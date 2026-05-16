#include <cstdint>
#include <cstdio>
#include <algorithm>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "runtime_config.hpp"
#include "app_stats.hpp"
#include "wcan.hpp"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#define READ_DATA_TASK_PRIORITY 10

static void init_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void init_wifi()
{
    static const char *TAG = "WIFI";

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t primary_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary_channel, &second_channel));
    if (primary_channel != ESPNOW_CHANNEL || second_channel != WIFI_SECOND_CHAN_NONE) {
        ESP_LOGW(TAG, "Wi-Fi channel mismatch: expected primary=%u second=%d, got primary=%u second=%d",
                 static_cast<unsigned>(ESPNOW_CHANNEL),
                 static_cast<int>(WIFI_SECOND_CHAN_NONE),
                 static_cast<unsigned>(primary_channel),
                 static_cast<int>(second_channel));
    } else {
        ESP_LOGI(TAG, "Wi-Fi channel locked: primary=%u second=%d",
                 static_cast<unsigned>(primary_channel),
                 static_cast<int>(second_channel));
    }
}

static void log_mac()
{
    static const char *TAG = "MAIN";
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESPNOW_MAC_TYPE));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void read_data_task(void *pv_parameter)
{
    static const char *TAG = "read_data_task";
    const int hz = static_cast<int>(reinterpret_cast<intptr_t>(pv_parameter));
    uint32_t counter = 0;

    ESP_LOGI(TAG, "read_data_task started at %d Hz", hz);

    const int effective_hz = std::max(1, hz);
    const uint32_t period_ms = 1000 / effective_hz;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        // Enqueue every sample to all registered CAN ID queues
        for (size_t i = 0; i < num_can_queues; ++i) {
            const uint32_t can_id = tx_can_ids[i];
            if (xQueueSend(can_queues[i], &counter, 0) != pdTRUE) {
                ESP_LOGV(TAG, "[0x%lx] Send queue full, dropping counter=%lu",
                         static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            } else {
                std::printf("S:%lu:%lx:%lu\n", (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()), 
                         static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            }
        }

        counter++;
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(period_ms));
    }
}

void SetupSensor(uint32_t base_can_id, size_t active_count, int sensor_hz, uint32_t linger_ms)
{
    static const char *TAG = "SENSOR_APP";

    ESP_LOGI(TAG, "Setting up SENSOR mode - Base ID: 0x%lx, Count: %u, Hz: %d",
             static_cast<unsigned long>(base_can_id), static_cast<unsigned>(active_count), sensor_hz);

    const uint32_t jitter_ms = esp_random() % 1000;
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    uint32_t tx_ids[app_config::kMaxCanIds];
    for (size_t i = 0; i < active_count; ++i) {
        tx_ids[i] = base_can_id + i;
    }
    wcan_init(true, nullptr, 0, tx_ids, active_count, linger_ms);

    xTaskCreate(read_data_task, "read_data_task", 4096, reinterpret_cast<void *>(static_cast<intptr_t>(sensor_hz)), 
                READ_DATA_TASK_PRIORITY, nullptr);
}

void wcan_recv_callback(const data_packet_t &recv_packet)
{
    if (recv_packet.data_count == 0) {
        return;
    }

    app_stats_detail::RecordPacketStats(recv_packet);

#ifdef MEASURE_INSTR
    static bool s_first_rx_logged = false;
    if (!s_first_rx_logged) {
        s_first_rx_logged = true;
        ESP_LOGI("FIRST_RX_TS", "us=%lld id=0x%lx", esp_timer_get_time(),
                 static_cast<unsigned long>(recv_packet.can_id));
    }
#endif

    std::printf("R:%lu:%lx:%lu:%lu:%lu:%u\n",
             (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
             static_cast<unsigned long>(recv_packet.can_id),
             static_cast<unsigned long>(recv_packet.tick_count), 
             static_cast<unsigned long>(recv_packet.data[0]),
             static_cast<unsigned long>(recv_packet.data[recv_packet.data_count - 1]), 
             recv_packet.data_count);
}

void SetupReceiver(bool filter_enabled, const uint32_t *filter_ids, size_t filter_count)
{
    static const char *TAG = "RECEIVER_APP";

    if (filter_enabled) {
        ESP_LOGI(TAG, "Setting up RECEIVER mode - active filter with %u CAN ID(s)", static_cast<unsigned>(filter_count));
        wcan_init(true, const_cast<uint32_t *>(filter_ids), filter_count, nullptr, 0, 0);
    } else {
        ESP_LOGI(TAG, "Setting up RECEIVER mode - accepting all CAN IDs");
        wcan_init(false, nullptr, 0, nullptr, 0, 0);
    }
}

extern "C" void app_main(void)
{
#ifdef MEASURE_INSTR
    ESP_LOGI("BOOT_TS", "us=%lld", esp_timer_get_time());
#endif

    const runtime_config::RuntimeConfig config = runtime_config::WaitForBootConfig();
    if (config.role == runtime_config::Role::kIdle) {
        ESP_LOGI("MAIN", "IDLE mode - stopping");
        return;
    }

    init_nvs();
    init_wifi();
    log_mac();

    if (config.role == runtime_config::Role::kSensor) {
        SetupSensor(config.sensor_base_can_id, config.sensor_can_id_count, config.sensor_hz, config.linger_ms);
    } else if (config.role == runtime_config::Role::kReceiver) {
        SetupReceiver(config.receiver_filter_enabled, config.receiver_filter_ids.data(), config.receiver_filter_count);
    }

    start_app_stats();
}
