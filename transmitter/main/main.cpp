#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "runtime_config.hpp"
#include "app_stats.hpp"
#include "wcan.hpp"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#define READ_DATA_TASK_PRIORITY 10

#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_MAC_TYPE ESP_MAC_WIFI_STA

using ConfigContext = runtime_config::ConfigContext;

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
    esp_err_t event_loop_err = esp_event_loop_create_default();
    if (event_loop_err != ESP_OK && event_loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_loop_err);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ConfigContext::kEspNowChannel, WIFI_SECOND_CHAN_NONE));

    uint8_t primary_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary_channel, &second_channel));
    if (primary_channel != ConfigContext::kEspNowChannel || second_channel != WIFI_SECOND_CHAN_NONE) {
        ESP_LOGW(TAG, "Wi-Fi channel mismatch: expected primary=%u second=%d, got primary=%u second=%d",
                 static_cast<unsigned>(ConfigContext::kEspNowChannel),
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

struct AppContext {
    ConfigContext config;
    std::unique_ptr<wcan::Transceiver> transceiver;
};

static AppContext s_app;

void read_data_task(void *pv_parameter)
{
    static const char *TAG = "read_data_task";
    auto* app = static_cast<AppContext*>(pv_parameter);
    if (!app->transceiver) {
        ESP_LOGE(TAG, "Transceiver not initialized");
        vTaskDelete(nullptr);
    }
    uint32_t counter = 0;

    ESP_LOGI(TAG, "read_data_task started at %d Hz", app->config.sensor_hz);

    const int effective_hz = std::max(1, app->config.sensor_hz);
    const uint32_t period_ms = 1000 / effective_hz;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        for (uint32_t can_id : app->transceiver->get_tx_can_ids()) {
            if (!app->transceiver->send_data(can_id, counter)) {
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

void wcan_recv_callback(const wcan::Packet &recv_packet)
{
    const auto& data = recv_packet.get_data();
    if (data.empty()) {
        return;
    }

    app_stats_detail::RecordPacketStats(recv_packet);

#ifdef MEASURE_INSTR
    static bool s_first_rx_logged = false;
    if (!s_first_rx_logged) {
        s_first_rx_logged = true;
        ESP_LOGI("FIRST_RX_TS", "us=%lld id=0x%lx", esp_timer_get_time(),
                 static_cast<unsigned long>(recv_packet.get_can_id()));
    }
#endif

    std::printf("R:%lu:%lx:%lu:%lu:%lu:%u\n",
             (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
             static_cast<unsigned long>(recv_packet.get_can_id()),
             static_cast<unsigned long>(recv_packet.get_sequence_id()),
             static_cast<unsigned long>(data.front()),
             static_cast<unsigned long>(data.back()),
             (unsigned int)data.size());
}

bool SetupSensor(AppContext &app)
{
    static const char *TAG = "SENSOR_APP";

    ESP_LOGI(TAG, "Setting up SENSOR mode - Base ID: 0x%lx, Count: %u, Hz: %d",
             static_cast<unsigned long>(app.config.sensor_base_can_id),
             static_cast<unsigned>(app.config.sensor_can_id_count),
             app.config.sensor_hz);

    const uint32_t jitter_ms = esp_random() % 1000;
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    std::vector<uint32_t> tx_ids;
    for (size_t i = 0; i < app.config.sensor_can_id_count; ++i) {
        tx_ids.push_back(app.config.sensor_base_can_id + i);
    }

    auto transceiver = std::make_unique<wcan::Transceiver>(std::vector<uint32_t>{}, std::move(tx_ids), app.config.linger_ms);
    transceiver->set_recv_callback(wcan_recv_callback);
    if (!transceiver->init()) {
        ESP_LOGE(TAG, "Failed to initialize Transceiver");
        return false;
    }

    app.transceiver = std::move(transceiver);
    xTaskCreate(read_data_task, "read_data_task", 4096, &app, READ_DATA_TASK_PRIORITY, nullptr);

    return true;
}

bool SetupReceiver(AppContext &app)
{
    static const char *TAG = "RECEIVER_APP";

    std::vector<uint32_t> rx_ids;
    if (app.config.receiver_filter_enabled) {
        ESP_LOGI(TAG, "Setting up RECEIVER mode - active filter with %u CAN ID(s)",
                 static_cast<unsigned>(app.config.receiver_filter_count));
        for (size_t i = 0; i < app.config.receiver_filter_count; ++i) {
            rx_ids.push_back(app.config.receiver_filter_ids[i]);
        }
    } else {
        ESP_LOGI(TAG, "Setting up RECEIVER mode - accepting all CAN IDs");
    }

    auto transceiver = std::make_unique<wcan::Transceiver>(std::move(rx_ids), std::vector<uint32_t>{}, 0);
    transceiver->set_recv_callback(wcan_recv_callback);
    if (!transceiver->init()) {
        ESP_LOGE(TAG, "Failed to initialize Transceiver");
        return false;
    }

    app.transceiver = std::move(transceiver);
    return true;
}

extern "C" void app_main(void)
{
#ifdef MEASURE_INSTR
    ESP_LOGI("BOOT_TS", "us=%lld", esp_timer_get_time());
#endif

    s_app.config = runtime_config::WaitForBootConfig();
    if (s_app.config.role == runtime_config::Role::kIdle) {
        ESP_LOGI("MAIN", "IDLE mode - stopping");
        return;
    }

    init_nvs();
    init_wifi();
    log_mac();

    bool setup_ok = false;
    if (s_app.config.role == runtime_config::Role::kSensor) {
        setup_ok = SetupSensor(s_app);
    } else if (s_app.config.role == runtime_config::Role::kReceiver) {
        setup_ok = SetupReceiver(s_app);
    }

    if (!setup_ok) {
        ESP_LOGE("MAIN", "Application setup failed");
        return;
    }

    start_app_stats();
}
