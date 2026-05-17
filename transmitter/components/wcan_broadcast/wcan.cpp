#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wcan.hpp"
#include "wcan_receiver.hpp"
#include "wcan_sender.hpp"
#include "wcan_utils.hpp"

bool recv_filter = false;
uint32_t *rx_can_ids = nullptr;
size_t rx_can_ids_size = 0;
uint32_t linger_ms = 0;
uint32_t *tx_can_ids = nullptr;
uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];
size_t num_can_queues = 0;

static std::unique_ptr<uint32_t[]> s_rx_can_ids_storage;
static std::unique_ptr<uint32_t[]> s_tx_can_ids_storage;

static constexpr UBaseType_t kSendProcessingTaskPriority = 6;
static constexpr UBaseType_t kRecvProcessingTaskPriority = 6;
static constexpr UBaseType_t kCanProcessingTaskPriority = 5;
static constexpr UBaseType_t kHeapMonitorTaskPriority = 1;

static bool copy_can_ids(const char *name, const uint32_t *ids, size_t ids_size, std::unique_ptr<uint32_t[]> &storage,
                         uint32_t *&target)
{
    static const char *TAG = "WCAN";

    storage.reset();
    target = nullptr;

    if (ids_size == 0) {
        return true;
    }
    if (ids == nullptr) {
        ESP_LOGE(TAG, "%s is NULL with size %u", name, static_cast<unsigned>(ids_size));
        return false;
    }

    storage = std::make_unique<uint32_t[]>(ids_size);
    if (!storage) {
        ESP_LOGE(TAG, "Failed to allocate %s storage", name);
        return false;
    }

    std::memcpy(storage.get(), ids, ids_size * sizeof(uint32_t));
    target = storage.get();
    return true;
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    static const char *TAG = "SEND_CB";
    if (mac_addr == nullptr) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    if (status == ESP_NOW_SEND_FAIL) {
        ESP_LOGW(TAG, "MAC-layer TX failed (no 802.11 ACK from peer)");
    } else {
        ESP_LOGI(TAG, "Successfully sent packet to %02x%02x", mac_addr[4], mac_addr[5]);
    }

    sender_on_send_status(status);
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    static const char *TAG = "RECV";

    if (recv_info->src_addr == nullptr || data == nullptr || data_len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    ESP_LOGD(TAG, "Received payload of size %d from %02x:%02x:%02x:%02x:%02x:%02x", data_len, recv_info->src_addr[0],
             recv_info->src_addr[1], recv_info->src_addr[2], recv_info->src_addr[3], recv_info->src_addr[4],
             recv_info->src_addr[5]);

    data_packet_t recv_data;
    if (!decode_data_packet(recv_info->src_addr, data, static_cast<size_t>(data_len), recv_data)) {
        ESP_LOGE(TAG, "decode_data_packet failed");
        return;
    }

    ESP_LOGD(TAG, "Received data with id: %08lx", static_cast<unsigned long>(recv_data.can_id));
    if (recv_data.can_id == CAN_ACK) {
        ack_recv(recv_data);
        // recv_data destructs at function exit; payload is freed.
    } else {
        auto heap_pkt = std::make_unique<data_packet_t>(std::move(recv_data));
        filter_data(std::move(heap_pkt));
    }
}

static bool create_handles(void)
{
    static const char *TAG = "WCAN";

    if (num_can_queues > 0) {
        can_queues = static_cast<QueueHandle_t *>(malloc(num_can_queues * sizeof(QueueHandle_t)));
        if (can_queues == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN queues");
            return false;
        }

        can_tx_tasks = static_cast<TaskHandle_t *>(calloc(num_can_queues, sizeof(TaskHandle_t)));
        if (can_tx_tasks == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN task handles");
            free(can_queues);
            can_queues = nullptr;
            return false;
        }

        can_tx_packets = static_cast<data_packet_t **>(malloc(num_can_queues * sizeof(data_packet_t *)));
        if (can_tx_packets == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN packet slots");
            free(can_queues);
            can_queues = nullptr;
            free(can_tx_tasks);
            can_tx_tasks = nullptr;
            return false;
        }

        can_tx_tick_counts = static_cast<volatile TickType_t *>(calloc(num_can_queues, sizeof(TickType_t)));
        if (can_tx_tick_counts == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN tick count slots");
            free(can_queues);
            can_queues = nullptr;
            free(can_tx_tasks);
            can_tx_tasks = nullptr;
            free(can_tx_packets);
            can_tx_packets = nullptr;
            return false;
        }
    }

    espnow_tx_sem = xSemaphoreCreateBinary();
    if (espnow_tx_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create espnow_tx_sem");
        return false;
    }
    xSemaphoreGive(espnow_tx_sem);

    espnow_tx_status_sem = xSemaphoreCreateBinary();
    if (espnow_tx_status_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create espnow_tx_status_sem");
        return false;
    }

    send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(void *));
    if (send_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create send queue");
        return false;
    }

    recv_queue = xQueueCreate(RECV_QUEUE_SIZE, sizeof(data_packet_t *));
    if (recv_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create receive queue");
        return false;
    }

    for (size_t i = 0; i < num_can_queues; i++) {
        can_queues[i] = xQueueCreate(RECV_QUEUE_SIZE, sizeof(uint32_t));
        if (can_queues[i] == nullptr) {
            ESP_LOGE(TAG, "Failed to create CAN queue %u", static_cast<unsigned>(i));
            return false;
        }
        can_tx_packets[i] = nullptr;
    }

    return true;
}

void wcan_init(bool filter, uint32_t *rx_ids, size_t rx_ids_size, uint32_t *tx_ids, size_t tx_ids_size, uint32_t linger)
{
    static const char *TAG = "WCAN";
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGV(TAG, "ESP-NOW initialized");
    add_peer(BROADCAST_MAC);
    ESP_LOGV(TAG, "Broadcast peer added");

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESPNOW_MAC_TYPE));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    std::memcpy(own_mac_addr, mac, ESP_NOW_ETH_ALEN);

    recv_filter = filter;

    if (tx_ids_size > 0 && tx_ids == nullptr) {
        ESP_LOGE(TAG, "tx_can_ids is NULL with size %u", static_cast<unsigned>(tx_ids_size));
        return;
    }
    if (filter && rx_ids_size > 0 && rx_ids == nullptr) {
        ESP_LOGE(TAG, "rx_can_ids is NULL with size %u", static_cast<unsigned>(rx_ids_size));
        return;
    }

    for (size_t i = 0; i < tx_ids_size; i++) {
        if (tx_ids[i] > CAN_ID_MAX) {
            ESP_LOGE(TAG, "tx_can_ids[%u] = 0x%08lx exceeds 29-bit CAN extended ID limit", static_cast<unsigned>(i),
                     static_cast<unsigned long>(tx_ids[i]));
            return;
        }
    }
    if (filter) {
        for (size_t i = 0; i < rx_ids_size; i++) {
            if (rx_ids[i] > CAN_ID_MAX) {
                ESP_LOGE(TAG, "rx_can_ids[%u] = 0x%08lx exceeds 29-bit CAN extended ID limit", static_cast<unsigned>(i),
                         static_cast<unsigned long>(rx_ids[i]));
                return;
            }
        }
    }

    if (!copy_can_ids("tx_can_ids", tx_ids, tx_ids_size, s_tx_can_ids_storage, tx_can_ids)) {
        return;
    }
    if (filter) {
        if (!copy_can_ids("rx_can_ids", rx_ids, rx_ids_size, s_rx_can_ids_storage, rx_can_ids)) {
            return;
        }
        rx_can_ids_size = rx_ids_size;
    } else {
        s_rx_can_ids_storage.reset();
        rx_can_ids = nullptr;
        rx_can_ids_size = 0;
    }

    linger_ms = linger;
    num_can_queues = tx_ids_size;

    if (!create_handles()) {
        ESP_LOGE(TAG, "Failed to create RTOS handles - aborting init");
        return;
    }

    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_LOGV(TAG, "ESP-NOW send callback registered");
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_LOGV(TAG, "ESP-NOW receive callback registered");

    xTaskCreate(send_processing_task, "send_processing_task", 4096, nullptr, kSendProcessingTaskPriority, nullptr);
    // Skip recv_processing_task when filter=true with an empty allowlist: ACKs are
    // routed directly through ack_recv and nothing else will ever reach the queue.
    if (!(filter && rx_ids_size == 0)) {
        if (!wcan_recv_callback) {
            ESP_LOGE(TAG, "wcan_recv_callback is not defined - recv_processing_task will not start. "
                          "Define wcan_recv_callback or use filter=true with an empty allowlist.");
            return;
        }
        xTaskCreate(recv_processing_task, "recv_processing_task", 4096, nullptr, kRecvProcessingTaskPriority, nullptr);
    }

    for (size_t i = 0; i < num_can_queues; i++) {
        char task_name[20];
        std::snprintf(task_name, sizeof(task_name), "can_proc_%u", static_cast<unsigned>(i));
        xTaskCreate(can_processing_task, task_name, 4096, reinterpret_cast<void *>(i), kCanProcessingTaskPriority,
                    &can_tx_tasks[i]);
    }

    ESP_LOGI(TAG, "WCAN initialized");
}
