#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan.hpp"
#include "wcan_sender.hpp"
#include "wcan_utils.hpp"

const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t send_queue = nullptr;
QueueHandle_t *can_queues = nullptr;
TaskHandle_t *can_tx_tasks = nullptr;
data_packet_t **can_tx_packets = nullptr;
volatile TickType_t *can_tx_tick_counts = nullptr;

SemaphoreHandle_t espnow_tx_sem = nullptr;
SemaphoreHandle_t espnow_tx_status_sem = nullptr;

struct tx_request_t {
    std::unique_ptr<esp_now_packet_t> packet;
    SemaphoreHandle_t completion_sem;
    bool *completion_success;
};

static SemaphoreHandle_t s_in_flight_completion_sem = nullptr;
static bool *s_in_flight_completion_success = nullptr;
static volatile bool s_in_flight_active = false;
static volatile bool s_in_flight_success = false;

static void log_send_error(const char *tag, esp_err_t err)
{
    if (err == ESP_ERR_ESPNOW_CHAN) {
        ESP_LOGE(tag, "esp_now_send failed: ESP_ERR_ESPNOW_CHAN (Wi-Fi channel mismatch)");
    } else if (err == ESP_ERR_ESPNOW_IF) {
        ESP_LOGE(tag, "esp_now_send failed: ESP_ERR_ESPNOW_IF (Wi-Fi interface mismatch)");
    } else {
        ESP_LOGE(tag, "esp_now_send failed synchronously: %s", esp_err_to_name(err));
    }
}

#ifdef MEASURE_INSTR
// Mirrors the airtime accounting in wcan_multicast for fair cross-variant
// comparison. Same constant + formula so the bias is identical.
#define WCAN_PHY_RATE_MBPS 6
volatile uint64_t g_airtime_total_us = 0;
volatile uint64_t g_packets_sent_total = 0;
#endif

static std::unique_ptr<data_packet_t> collect_packet(size_t can_queue_index, uint32_t can_id)
{
    uint32_t data_point[WCAN_DATA_PACKET_MAX_DATA_COUNT];

    if (xQueueReceive(can_queues[can_queue_index], &data_point[0], portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    size_t count = 1;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(linger_ms);
    while (count < WCAN_DATA_PACKET_MAX_DATA_COUNT) {
        const TickType_t now = xTaskGetTickCount();
        // avoids overflow issues with tick count wraparound
        if (static_cast<int32_t>(deadline - now) <= 0) {
            break;
        }
        const TickType_t remaining = deadline - now;
        if (xQueueReceive(can_queues[can_queue_index], &data_point[count], remaining) != pdTRUE) {
            break;
        }
        count++;
    }

    auto packet = std::make_unique<data_packet_t>();
    std::memcpy(packet->mac_addr.data(), own_mac_addr, ESP_NOW_ETH_ALEN);
    packet->can_id = can_id;
    packet->data_count = static_cast<uint8_t>(count);
    packet->tick_count = xTaskGetTickCount();
    packet->data = std::make_unique<uint32_t[]>(count);
    if (!packet->data) {
        return nullptr;
    }
    std::memcpy(packet->data.get(), data_point, count * sizeof(uint32_t));
    return packet;
}

static bool send_with_retry(size_t can_queue_index, const data_packet_t &packet)
{
    char TAG[20];
    std::snprintf(TAG, sizeof(TAG), "RESEND_%u", static_cast<unsigned>(can_queue_index));

    for (int attempt = 0; attempt < WCAN_MAX_RETRY_COUNT; attempt++) {
        send_data(BROADCAST_MAC, packet);

        const uint32_t delay =
            WCAN_RETRY_DELAY_MIN + (esp_random() % (WCAN_RETRY_DELAY_MAX - WCAN_RETRY_DELAY_MIN + 1));
        if (xTaskNotifyWait(0xFFFFFFFFUL, 0, nullptr, pdMS_TO_TICKS(delay)) == pdTRUE) {
            return true;
        }
        ESP_LOGW(TAG, "Timeout reached, resending %08lx... Attempt: %d of %d",
                 static_cast<unsigned long>(packet.can_id), attempt + 1, WCAN_MAX_RETRY_COUNT);
    }
    return false;
}

void can_processing_task(void *pv_parameter)
{
    const size_t can_queue_index = reinterpret_cast<size_t>(pv_parameter);

    char TAG[20];
    std::snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", static_cast<unsigned>(can_queue_index));

    const uint32_t can_id = get_can_id_from_queue_index(can_queue_index);

    ESP_LOGI(TAG, "CAN processing task %u started", static_cast<unsigned>(can_queue_index));

    while (true) {
        auto packet = collect_packet(can_queue_index, can_id);
        if (!packet) {
            continue;
        }

        can_tx_tick_counts[can_queue_index] = packet->tick_count;
        can_tx_packets[can_queue_index] = packet.get();

        ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu)", static_cast<unsigned long>(packet->can_id),
                 packet->data_count, static_cast<unsigned long>(packet->data[0]),
                 static_cast<unsigned long>(packet->data[packet->data_count - 1]),
                 static_cast<unsigned long>(packet->tick_count));

        if (!send_with_retry(can_queue_index, *packet)) {
            ESP_LOGE(TAG, "Max retry attempts reached for %08lx", static_cast<unsigned long>(packet->can_id));
        }

        can_tx_packets[can_queue_index] = nullptr;
        // packet destructor releases the payload + struct.
    }
}

void send_processing_task(void *)
{
    static const char *TAG = "SEND";

    ESP_LOGI(TAG, "Send processing task started");

    while (true) {
        tx_request_t *raw = nullptr;
        if (xQueueReceive(send_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        std::unique_ptr<tx_request_t> req(raw);
        std::unique_ptr<esp_now_packet_t> &pkt = req->packet;

        if (!pkt) {
            ESP_LOGE(TAG, "TX request missing packet");
            if (req->completion_success != nullptr) {
                *req->completion_success = false;
            }
            if (req->completion_sem != nullptr) {
                xSemaphoreGive(req->completion_sem);
            }
            continue;
        }

        if (xSemaphoreTake(espnow_tx_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "TX semaphore timeout - driver may be stuck, dropping packet");
            if (req->completion_success != nullptr) {
                *req->completion_success = false;
            }
            if (req->completion_sem != nullptr) {
                xSemaphoreGive(req->completion_sem);
            }
            continue; // pkt destructor frees
        }

        s_in_flight_completion_sem = req->completion_sem;
        s_in_flight_completion_success = req->completion_success;
        s_in_flight_success = false;
        s_in_flight_active = true;
        while (xSemaphoreTake(espnow_tx_status_sem, 0) == pdTRUE) {
        }

        const esp_err_t err = esp_now_send(pkt->mac_addr.data(), pkt->data.get(), pkt->data_len);
        if (err != ESP_OK) {
            log_send_error(TAG, err);
            if (s_in_flight_completion_success != nullptr) {
                *s_in_flight_completion_success = false;
            }
            if (s_in_flight_completion_sem != nullptr) {
                xSemaphoreGive(s_in_flight_completion_sem);
            }
            s_in_flight_completion_sem = nullptr;
            s_in_flight_completion_success = nullptr;
            s_in_flight_active = false;
            s_in_flight_success = false;
            xSemaphoreGive(espnow_tx_sem);
            continue;
        }
#ifdef MEASURE_INSTR
        else {
            g_airtime_total_us += (static_cast<uint64_t>(pkt->data_len) * 8) / WCAN_PHY_RATE_MBPS;
            g_packets_sent_total = g_packets_sent_total + 1;
        }
#endif

        const bool got_status =
            (xSemaphoreTake(espnow_tx_status_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) == pdTRUE);
        const bool success = got_status && s_in_flight_success;
        if (s_in_flight_completion_success != nullptr) {
            *s_in_flight_completion_success = success;
        }
        if (s_in_flight_completion_sem != nullptr) {
            xSemaphoreGive(s_in_flight_completion_sem);
        }
        s_in_flight_completion_sem = nullptr;
        s_in_flight_completion_success = nullptr;
        s_in_flight_active = false;
        s_in_flight_success = false;
        xSemaphoreGive(espnow_tx_sem);
        // pkt destructor releases the payload + struct.
    }
}

void send_data(const uint8_t *mac_addr, const data_packet_t &data_packet)
{
    static const char *TAG = "send_data";

    auto pkt = encode_data_packet(data_packet);
    if (!pkt) {
        ESP_LOGE(TAG, "encode_data_packet failed, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        return;
    }
    std::memcpy(pkt->mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);

    auto req = std::make_unique<tx_request_t>();
    req->packet = std::move(pkt);
    req->completion_sem = nullptr;
    req->completion_success = nullptr;

    tx_request_t *raw = req.release();
    if (xQueueSend(send_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        delete raw;
    }
}

bool send_data_and_wait(const uint8_t *mac_addr, const data_packet_t &data_packet)
{
    static const char *TAG = "send_data_wait";

    auto pkt = encode_data_packet(data_packet);
    if (!pkt) {
        ESP_LOGE(TAG, "encode_data_packet failed, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        return false;
    }
    std::memcpy(pkt->mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);

    auto req = std::make_unique<tx_request_t>();
    bool success = false;
    SemaphoreHandle_t completion_sem = xSemaphoreCreateBinary();
    if (completion_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create completion semaphore");
        return false;
    }

    req->packet = std::move(pkt);
    req->completion_sem = completion_sem;
    req->completion_success = &success;

    tx_request_t *raw = req.release();
    if (xQueueSend(send_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        delete raw;
        vSemaphoreDelete(completion_sem);
        return false;
    }

    const bool completed = (xSemaphoreTake(completion_sem, portMAX_DELAY) == pdTRUE);
    vSemaphoreDelete(completion_sem);
    return completed && success;
}

void sender_on_send_status(esp_now_send_status_t status)
{
    if (!s_in_flight_active) {
        return;
    }
    s_in_flight_success = (status == ESP_NOW_SEND_SUCCESS);
    if (espnow_tx_status_sem != nullptr) {
        xSemaphoreGive(espnow_tx_status_sem);
    }
}

void ack_recv(const data_packet_t &recv_data)
{
    static const char *TAG = "ACK";

    if (!recv_data.data || recv_data.data_count < 1) {
        ESP_LOGW(TAG, "Malformed ACK: missing payload");
        return;
    }

    const uint32_t acked_can_id = recv_data.data[0];

    const size_t can_queue_index = get_can_tx_queue_index(acked_can_id);
    if (can_queue_index == SIZE_MAX) {
        ESP_LOGW(TAG, "Received ACK for unknown CAN ID 0x%08lx", static_cast<unsigned long>(acked_can_id));
        return;
    }

    if (can_tx_packets[can_queue_index] == nullptr) {
        ESP_LOGW(TAG, "Duplicate ACK ignored for 0x%08lx", static_cast<unsigned long>(acked_can_id));
        return;
    }

    if (recv_data.tick_count != can_tx_tick_counts[can_queue_index]) {
        ESP_LOGW(TAG, "[0x%08lx] with tick_count %lu, but expected tick_count %lu",
                 static_cast<unsigned long>(acked_can_id), static_cast<unsigned long>(recv_data.tick_count),
                 static_cast<unsigned long>(can_tx_tick_counts[can_queue_index]));
        return;
    }

    ESP_LOGD(TAG, "Received ACK for packet tick %lu from %02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned long>(recv_data.tick_count), recv_data.mac_addr[0], recv_data.mac_addr[1],
             recv_data.mac_addr[2], recv_data.mac_addr[3], recv_data.mac_addr[4], recv_data.mac_addr[5]);

#ifdef MEASURE_INSTR
    {
        const TickType_t now = xTaskGetTickCount();
        const uint32_t rtt_ms =
            static_cast<uint32_t>(pdTICKS_TO_MS(now - can_tx_tick_counts[can_queue_index]));
        ESP_LOGI("LAT_RTT", "id=0x%08lx rtt_ms=%lu peer=%02x:%02x:%02x:%02x:%02x:%02x",
                 static_cast<unsigned long>(acked_can_id), static_cast<unsigned long>(rtt_ms),
                 recv_data.mac_addr[0], recv_data.mac_addr[1], recv_data.mac_addr[2],
                 recv_data.mac_addr[3], recv_data.mac_addr[4], recv_data.mac_addr[5]);
    }
#endif

    if (can_tx_tasks[can_queue_index] != nullptr) {
        xTaskNotify(can_tx_tasks[can_queue_index], 0, eNoAction);
        ESP_LOGV(TAG, "Sender task notified");
    }
}
