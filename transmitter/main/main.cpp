#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "runtime_config.hpp"
#include "wcan.hpp"

#include "esp_timer.h"

#define ESPNOW_WIFI_MODE WIFI_MODE_STA

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
    }
}

struct AppContext {
    ConfigContext config;
    std::unique_ptr<wcan::Transceiver> transceiver;
    esp_timer_handle_t sensor_timer = nullptr;
    uint32_t generated_data_point = 0;
};

static AppContext s_app;

void sensor_timer_callback(void *pv_parameter)
{
    auto* app = static_cast<AppContext*>(pv_parameter);
    if (!app->transceiver) {
        ESP_LOGE("sensor_timer", "Transceiver not initialized");
        return;
    }

    const uint32_t counter = app->generated_data_point++;
    for (uint32_t can_id : app->transceiver->get_tx_can_ids()) {
        if (!app->transceiver->send_data(can_id, counter)) {
            std::printf("S(FAIL):%lu:%lx:%lu\n", (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                        static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            std::fflush(stdout);
        }
    }
}

bool start_sensor_timer(AppContext &app)
{
    static const char *TAG = "sensor_timer";
    const int effective_hz = std::max(1, app.config.sensor_hz);
    const uint64_t period_us = std::max<uint64_t>(1, 1000000ULL / static_cast<uint64_t>(effective_hz));
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = sensor_timer_callback;
    timer_args.arg = &app;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "sensor_timer";
    timer_args.skip_unhandled_events = true;

    esp_err_t err = esp_timer_create(&timer_args, &app.sensor_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create sensor timer: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_timer_start_periodic(app.sensor_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sensor timer: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

void stop_sensor_timer(AppContext &app)
{
    if (app.sensor_timer == nullptr) {
        return;
    }
    esp_timer_stop(app.sensor_timer);
    esp_timer_delete(app.sensor_timer);
    app.sensor_timer = nullptr;
}


bool SetupSensor(AppContext &app)
{
    std::vector<uint32_t> tx_ids;
    for (size_t i = 0; i < app.config.sensor_can_id_count; ++i) {
        tx_ids.push_back(app.config.sensor_base_can_id + i);
    }

    auto transceiver = std::make_unique<wcan::Transceiver>(std::vector<uint32_t>{}, std::move(tx_ids), app.config.linger_ms, true);
    if (!transceiver->init()) {
        ESP_LOGE("SENSOR_APP", "Failed to initialize Transceiver");
        return false;
    }

    app.transceiver = std::move(transceiver);
    return true;
}

bool SetupReceiver(AppContext &app)
{
    std::vector<uint32_t> rx_ids;
    if (app.config.receiver_filter_enabled) {
        for (size_t i = 0; i < app.config.receiver_filter_count; ++i) {
            rx_ids.push_back(app.config.receiver_filter_ids[i]);
        }
    }

    auto transceiver = std::make_unique<wcan::Transceiver>(std::move(rx_ids), std::vector<uint32_t>{}, 0, app.config.receiver_filter_enabled);
    if (!transceiver->init()) {
        ESP_LOGE("RECEIVER_APP", "Failed to initialize Transceiver");
        return false;
    }

    app.transceiver = std::move(transceiver);
    return true;
}

static uint32_t StopDrainTimeoutMs(const ConfigContext& config)
{
    return std::max<uint32_t>(1, std::min<uint32_t>(1000, config.host_wait_time_ms / 2));
}


static void RunTimedTest(AppContext& app)
{
    runtime_config::WaitForTestStart();
    if (app.transceiver) {
        app.transceiver->stats().reset();
    }
    if (app.config.role == runtime_config::Role::kSensor && !start_sensor_timer(app)) {
        std::printf("WCAN_TEST_ABORT role=sensor reason=sensor-timer\n");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(app.config.test_duration_ms));
    if (app.transceiver) {
        app.transceiver->stats().finish_test();
    }

    stop_sensor_timer(app);
    if (app.transceiver) {
        app.transceiver->stop(StopDrainTimeoutMs(app.config));
        app.transceiver->stats().print_batch_stats();
    }

    if (app.config.role == runtime_config::Role::kSensor && app.transceiver) {
        app.transceiver->stats().print_sensor_end(app.generated_data_point);
    }
    if (app.config.role == runtime_config::Role::kReceiver && app.transceiver) {
        app.transceiver->stats().print_rx_ranges();
    }
    if (app.transceiver) {
        app.transceiver->stats().print_measures();
    }
    std::printf("WCAN_TEST_END role=%s\n", runtime_config::RoleName(app.config.role));
    std::fflush(stdout);
}

extern "C" void app_main(void)
{
    s_app.config = runtime_config::WaitForBootConfig();

    if (s_app.config.role == runtime_config::Role::kIdle) {
        std::printf("WCAN_TEST_READY role=%s\n", runtime_config::RoleName(s_app.config.role));
        std::fflush(stdout);
        runtime_config::WaitForTestStart();
        return;
    }

    init_nvs();
    init_wifi();

    bool setup_ok = false;
    if (s_app.config.role == runtime_config::Role::kSensor) {
        setup_ok = SetupSensor(s_app);
    } else if (s_app.config.role == runtime_config::Role::kReceiver) {
        setup_ok = SetupReceiver(s_app);
    }

    if (!setup_ok) {
        std::printf("WCAN_TEST_ABORT role=%s reason=setup\n", runtime_config::RoleName(s_app.config.role));
        std::fflush(stdout);
        return;
    }

    std::printf("WCAN_TEST_READY role=%s\n", runtime_config::RoleName(s_app.config.role));
    std::fflush(stdout);
    RunTimedTest(s_app);
}
