#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan.hpp"
#include "wcan_sender.hpp"
#include "wcan_subscriptions.hpp"
#include "wcan_utils.hpp"

const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t send_queue = nullptr;
QueueHandle_t *can_queues = nullptr;

SemaphoreHandle_t espnow_tx_sem = nullptr;

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

void can_processing_task(void *pv_parameter)
{
    const size_t can_queue_index = reinterpret_cast<size_t>(pv_parameter);

    char TAG[20];
    std::snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", static_cast<unsigned>(can_queue_index));

    const uint32_t can_id = get_can_id_from_queue_index(can_queue_index);

    ESP_LOGI(TAG, "CAN processing task %u started", static_cast<unsigned>(can_queue_index));

    uint8_t targets[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN];

    while (true) {
        auto packet = collect_packet(can_queue_index, can_id);
        if (!packet) {
            continue;
        }

        const size_t fan_out = subscription_snapshot_targets(can_id, targets);
        if (fan_out == 0) {
            ESP_LOGD(TAG, "Suppressing 0x%08lx (no alive subscribers)",
                     static_cast<unsigned long>(can_id));
            continue;
        }

        for (size_t i = 0; i < fan_out; i++) {
            add_peer(targets[i]);
            send_data(targets[i], *packet);
        }

        ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu) -> %u peers",
                 static_cast<unsigned long>(packet->can_id), packet->data_count,
                 static_cast<unsigned long>(packet->data[0]),
                 static_cast<unsigned long>(packet->data[packet->data_count - 1]),
                 static_cast<unsigned long>(packet->tick_count),
                 static_cast<unsigned>(fan_out));
        // packet destructor releases the payload + struct.
    }
}

void send_processing_task(void *)
{
    static const char *TAG = "SEND";

    ESP_LOGI(TAG, "Send processing task started");

    while (true) {
        esp_now_packet_t *raw = nullptr;
        if (xQueueReceive(send_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        std::unique_ptr<esp_now_packet_t> pkt(raw);

        if (xSemaphoreTake(espnow_tx_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "TX semaphore timeout - driver may be stuck, dropping packet");
            continue; // pkt destructor frees
        }

        const esp_err_t err = esp_now_send(pkt->mac_addr.data(), pkt->data.get(), pkt->data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send failed synchronously: %s - restoring TX slot", esp_err_to_name(err));
            xSemaphoreGive(espnow_tx_sem);
        }
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

    esp_now_packet_t *raw = pkt.release();
    if (xQueueSend(send_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full, dropping packet with CAN ID 0x%08lx",
                 static_cast<unsigned long>(data_packet.can_id));
        delete raw;
    }
}
