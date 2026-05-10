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

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#include "wcan.hpp"
#include "wcan_receiver.hpp"
#include "wcan_sender.hpp"
#include "wcan_subscriptions.hpp"
#include "wcan_utils.hpp"

bool recv_filter = false;
uint32_t *rx_can_ids = nullptr;
size_t rx_can_ids_size = 0;
uint32_t linger_ms = 0;
uint32_t *tx_can_ids = nullptr;
uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];
size_t num_can_queues = 0;

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    static const char *TAG = "SEND_CB";
    if (mac_addr == nullptr) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    const bool success = (status == ESP_NOW_SEND_SUCCESS);
    if (!success) {
        ESP_LOGW(TAG, "MAC-layer TX failed to %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    } else {
        ESP_LOGV(TAG, "Sent to %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    }

#ifdef MEASURE_INSTR
    {
        const int64_t now_us = esp_timer_get_time();
        const int64_t dt_us = now_us - g_in_flight_send_us;
        ESP_LOGI("LAT_CB", "peer=%02x:%02x:%02x:%02x:%02x:%02x dt_us=%lld status=%s",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
                 dt_us, success ? "OK" : "FAIL");
    }
#endif

    // Update per-peer TX health. No-op for non-subscriber MACs (e.g., broadcast).
    subscription_record_tx_status(mac_addr, success);

    if (s_in_flight_completion_success != nullptr) {
        *s_in_flight_completion_success = success;
    }
    if (s_in_flight_completion_sem != nullptr) {
        xSemaphoreGive(s_in_flight_completion_sem);
        s_in_flight_completion_sem = nullptr;
        s_in_flight_completion_success = nullptr;
    }

    if (espnow_tx_sem != nullptr) {
        xSemaphoreGive(espnow_tx_sem);
    } else {
        ESP_LOGE(TAG, "espnow_tx_sem is NULL in send callback - init order error");
    }
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
    recv_data.received_via_broadcast =
        (recv_info->des_addr != nullptr &&
         std::memcmp(recv_info->des_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN) == 0);

    ESP_LOGD(TAG, "Received data with id: %08lx", static_cast<unsigned long>(recv_data.can_id));
    if (recv_data.can_id == CAN_HELLO) {
        // HELLO carries a subscriber's CAN-ID interest list. data_count==0 means wildcard.
        // No-ops on nodes where subscription_init was not called (e.g., receiver-only).
        const uint32_t *ids = (recv_data.data_count > 0 && recv_data.data) ? recv_data.data.get() : nullptr;
        subscription_update(recv_data.mac_addr.data(), ids, recv_data.data_count);
        sender_on_subscription_update(ids, recv_data.data_count);
    } else {
        auto heap_pkt = std::make_unique<data_packet_t>(std::move(recv_data));
        filter_data(std::move(heap_pkt));
    }
}

#define WCAN_HEAP_MONITOR_INTERVAL_MS 5000

static void heap_monitor_task(void *)
{
    static const char *TAG = "HEAP";
    const TickType_t period = pdMS_TO_TICKS(WCAN_HEAP_MONITOR_INTERVAL_MS);
    while (true) {
        ESP_LOGI(TAG, "free=%u min_free=%u largest=%u", static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        vTaskDelay(period);
    }
}

#define WCAN_HELLO_BURST_COUNT 5
#define WCAN_HELLO_BURST_INTERVAL_MS 50
#define WCAN_HELLO_STEADY_INTERVAL_MS 1000

static void send_hello(void)
{
    data_packet_t hello;
    std::memcpy(hello.mac_addr.data(), own_mac_addr, ESP_NOW_ETH_ALEN);
    hello.can_id = CAN_HELLO;
    hello.tick_count = xTaskGetTickCount();

    if (recv_filter && rx_can_ids != nullptr && rx_can_ids_size > 0) {
        hello.data_count = static_cast<uint8_t>(rx_can_ids_size);
        hello.data = std::make_unique<uint32_t[]>(rx_can_ids_size);
        if (!hello.data) {
            return;
        }
        std::memcpy(hello.data.get(), rx_can_ids, rx_can_ids_size * sizeof(uint32_t));
    } else {
        hello.data_count = 0;
    }

    send_data(BROADCAST_MAC, hello);
}

static void hello_beacon_task(void *)
{
    static const char *TAG = "HELLO_TX";
    ESP_LOGI(TAG, "HELLO beacon task started (%s, %u ids)",
             recv_filter ? "selective" : "wildcard",
             static_cast<unsigned>(recv_filter ? rx_can_ids_size : 0));

    for (int i = 0; i < WCAN_HELLO_BURST_COUNT; i++) {
        send_hello();
        vTaskDelay(pdMS_TO_TICKS(WCAN_HELLO_BURST_INTERVAL_MS));
    }

    const TickType_t period = pdMS_TO_TICKS(WCAN_HELLO_STEADY_INTERVAL_MS);
    while (true) {
        send_hello();
        vTaskDelay(period);
    }
}

#define WCAN_SUBS_LOG_INTERVAL_MS 10000

static void subscription_log_task(void *)
{
    const TickType_t period = pdMS_TO_TICKS(WCAN_SUBS_LOG_INTERVAL_MS);
    while (true) {
        subscription_log_state();
        vTaskDelay(period);
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
    }

    espnow_tx_sem = xSemaphoreCreateBinary();
    if (espnow_tx_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create espnow_tx_sem");
        return false;
    }
    xSemaphoreGive(espnow_tx_sem);

    send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(esp_now_packet_t *));
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
    if (filter) {
        rx_can_ids = rx_ids;
        rx_can_ids_size = rx_ids_size;
    } else {
        rx_can_ids = nullptr;
        rx_can_ids_size = 0;
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

    tx_can_ids = tx_ids;
    linger_ms = linger;
    num_can_queues = tx_ids_size;

    if (!create_handles()) {
        ESP_LOGE(TAG, "Failed to create RTOS handles - aborting init");
        return;
    }

    if (tx_ids_size > 0) {
        subscription_init();
        sender_init_delivery_state();
    }

    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_LOGV(TAG, "ESP-NOW send callback registered");
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_LOGV(TAG, "ESP-NOW receive callback registered");

    xTaskCreate(send_processing_task, "send_processing_task", 4096, nullptr, 5, nullptr);
    // Skip recv_processing_task when filter=true with an empty allowlist: ACKs are
    // routed directly through ack_recv and nothing else will ever reach the queue.
    if (!(filter && rx_ids_size == 0)) {
        if (!wcan_recv_callback) {
            ESP_LOGE(TAG, "wcan_recv_callback is not defined - recv_processing_task will not start. "
                          "Define wcan_recv_callback or use filter=true with an empty allowlist.");
            return;
        }
        xTaskCreate(recv_processing_task, "recv_processing_task", 4096, nullptr, 5, nullptr);
    }

    for (size_t i = 0; i < num_can_queues; i++) {
        char task_name[20];
        std::snprintf(task_name, sizeof(task_name), "can_proc_%u", static_cast<unsigned>(i));
        xTaskCreate(can_processing_task, task_name, 4096, reinterpret_cast<void *>(i), 4, nullptr);
    }

    xTaskCreate(heap_monitor_task, "heap_monitor", 2048, nullptr, 1, nullptr);

    // Sensor: maintain the subscription table populated by HELLO frames from receivers,
    // and periodically log it as a Phase-2 sanity check. Receivers keep beaconing and
    // run a directed registration retry loop after hearing a broadcast data packet.
    if (tx_ids_size > 0) {
        xTaskCreate(subscription_log_task, "subs_log", 2048, nullptr, 1, nullptr);
    } else {
        registration_init();
        xTaskCreate(hello_beacon_task, "hello_beacon", 4096, nullptr, 4, nullptr);
    }

#ifdef MEASURE_INSTR
    measure_start();
#endif

    ESP_LOGI(TAG, "WCAN initialized");
}
