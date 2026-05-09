#include <cstdint>
#include <cstdio>
#include <cstring>

#include <nvs_flash.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan.hpp"
#include "wcan_utils.hpp"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

// Compile-time role validation
#if !defined(ROLE_SENSOR) && !defined(ROLE_RECEIVER) && !defined(ROLE_IDLE)
#error                                                                                                                 \
    "Build must define ROLE=SENSOR or ROLE=RECEIVER or ROLE=IDLE via CMake (-DROLE=SENSOR or -DROLE=RECEIVER or -DROLE=IDLE)"
#endif

#ifdef ROLE_SENSOR
#ifndef SENSOR_HZ
#define SENSOR_HZ 200
#endif
static constexpr int FREQUENCY_HERTZ = SENSOR_HZ;
#endif

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

#ifdef ROLE_SENSOR
// Sensor task: increments a counter and sends it over WCAN at FREQUENCY_HERTZ.
static void read_data_task(void *pv_parameter)
{
    static const char *TAG = "read_data_task";
    const uint32_t can_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pv_parameter));
    const size_t can_queue_index = get_can_tx_queue_index(can_id);
    if (can_queue_index == SIZE_MAX) {
        ESP_LOGE(TAG, "Unknown CAN ID 0x%04lx, aborting task", static_cast<unsigned long>(can_id));
        vTaskDelete(nullptr);
        return;
    }
    uint32_t counter = 0;

    ESP_LOGI(TAG, "read_data_task started with CAN ID 0x%04lx at %d Hz", static_cast<unsigned long>(can_id),
             FREQUENCY_HERTZ);

    while (true) {
        if (xQueueSend(can_queues[can_queue_index], &counter, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Send queue full, dropping counter=%lu", static_cast<unsigned long>(counter));
        } else {
            ESP_LOGI(TAG, "%lu", static_cast<unsigned long>(counter));
        }
        counter++;

        vTaskDelay(pdMS_TO_TICKS(1000 / FREQUENCY_HERTZ));
    }
}
#endif // ROLE_SENSOR

#ifdef ROLE_RECEIVER
// Receiver callback: logs any incoming CAN ID and payload
void wcan_recv_callback(const data_packet_t &recv_packet)
{
    static const char *TAG = "wcan_recv_callback";
    if (recv_packet.data_count == 0) {
        return;
    }

#ifdef MEASURE_INSTR
    static bool s_first_rx_logged = false;
    if (!s_first_rx_logged) {
        s_first_rx_logged = true;
        ESP_LOGI("FIRST_RX_TS", "us=%lld id=0x%08lx",
                 esp_timer_get_time(), static_cast<unsigned long>(recv_packet.can_id));
    }
#endif

    ESP_LOGI(TAG, "[%04lx] tick=%lu [%lu..%lu] %u items", static_cast<unsigned long>(recv_packet.can_id),
             static_cast<unsigned long>(recv_packet.tick_count), static_cast<unsigned long>(recv_packet.data[0]),
             static_cast<unsigned long>(recv_packet.data[recv_packet.data_count - 1]), recv_packet.data_count);
}
#endif // ROLE_RECEIVER

extern "C" void app_main(void)
{
    static const char *TAG = "MAIN";

#ifdef MEASURE_INSTR
    ESP_LOGI("BOOT_TS", "us=%lld", esp_timer_get_time());
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESPNOW_MAC_TYPE));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

#if !defined(ROLE_IDLE)
    wifi_init();
    ESP_LOGI(TAG, "WiFi initialized");
#endif

#ifdef ROLE_SENSOR
    static uint32_t can_id = (static_cast<uint32_t>(mac[4]) << 8) | static_cast<uint32_t>(mac[5]);
    ESP_LOGI(TAG, "SENSOR mode - CAN ID: 0x%04lx", static_cast<unsigned long>(can_id));

    const uint32_t jitter_ms = esp_random() % 1000;
    ESP_LOGI(TAG, "Startup jitter: %lu ms", static_cast<unsigned long>(jitter_ms));
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    // filter=true + empty rx allowlist drops all non-ACK data; ACKs are routed
    // directly through ack_recv and do not need recv_processing_task.
    wcan_init(true, nullptr, 0, &can_id, 1, 100);

    xTaskCreate(read_data_task, "read_data_task", 4096, reinterpret_cast<void *>(static_cast<uintptr_t>(can_id)), 5,
                nullptr);

#elif defined(ROLE_RECEIVER)
    ESP_LOGI(TAG, "RECEIVER mode - accepting all CAN IDs");

    // filter=false means accept everything
    wcan_init(false, nullptr, 0, nullptr, 0, 0);

#elif defined(ROLE_IDLE)
    ESP_LOGI(TAG, "IDLE mode - doing nothing");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

#endif
}
