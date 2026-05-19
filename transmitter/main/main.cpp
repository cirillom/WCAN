#include <cstdio>
#include <memory>
#include <vector>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "RampCanSensor.hpp"
#include "WcanTest.hpp"
#include "wcan.hpp"

#define ESPNOW_WIFI_MODE WIFI_MODE_STA

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
    ESP_ERROR_CHECK(esp_wifi_set_channel(wcan_test::TestConfig::kEspNowChannel, WIFI_SECOND_CHAN_NONE));

    uint8_t primary_channel = 0;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary_channel, &second_channel));
    if (primary_channel != wcan_test::TestConfig::kEspNowChannel || second_channel != WIFI_SECOND_CHAN_NONE) {
        ESP_LOGW(TAG, "Wi-Fi channel mismatch: expected primary=%u second=%d, got primary=%u second=%d",
                 static_cast<unsigned>(wcan_test::TestConfig::kEspNowChannel),
                 static_cast<int>(WIFI_SECOND_CHAN_NONE),
                 static_cast<unsigned>(primary_channel),
                 static_cast<int>(second_channel));
    }
}

extern "C" void app_main(void)
{
    const wcan_test::TestConfig config = wcan_test::UartTestProtocol::wait_for_boot_config();
    const wcan_test::WcanTestSession test_session(config);

    if (config.role == wcan_test::Role::kIdle) {
        test_session.ready();
        test_session.wait_idle_start();
        return;
    }

    init_nvs();
    init_wifi();

    const bool is_sensor = config.role == wcan_test::Role::kSensor;
    const bool is_receiver = config.role == wcan_test::Role::kReceiver;

    std::unique_ptr<wcan::Transceiver> transceiver;
    if (is_sensor) {
        transceiver = std::make_unique<wcan::Transceiver>(
            std::vector<wcan::CANId_t>{},
            config.sensor_tx_ids(),
            config.linger_ms,
            true);
    } else if (is_receiver) {
        transceiver = std::make_unique<wcan::Transceiver>(
            config.receiver_rx_ids(),
            std::vector<wcan::CANId_t>{},
            0,
            config.receiver_filter_enabled);
    }

    if (!transceiver || !transceiver->init()) {
        test_session.abort("setup");
        return;
    }

    std::unique_ptr<wcan_sensor::RampCanSensor> sensor;
    if (is_sensor) {
        sensor = std::make_unique<wcan_sensor::RampCanSensor>(*transceiver);
    }

    test_session.ready();
    test_session.run(*transceiver, sensor.get());
}
