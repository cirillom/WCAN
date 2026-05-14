#include <cstdint>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "receiver_app.hpp"
#include "runtime_config.hpp"
#include "sensor_app.hpp"
#include "wcan.hpp"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

static void log_mac()
{
    static const char *TAG = "MAIN";
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESPNOW_MAC_TYPE));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

extern "C" void app_main(void)
{
#ifdef MEASURE_INSTR
    ESP_LOGI("BOOT_TS", "us=%lld", esp_timer_get_time());
#endif

    const runtime_config::RuntimeConfig config = runtime_config::WaitForBootConfig();
    if (config.role == runtime_config::Role::kIdle) {
        ESP_LOGI("MAIN", "IDLE mode - stopping before NVS/Wi-Fi initialization");
        return;
    }

    init_nvs();
    init_wifi();
    log_mac();

    if (config.role == runtime_config::Role::kSensor) {
        sensor_app::SetupSensor(config.sensor_base_can_id, config.sensor_can_id_count, config.sensor_hz,
                                config.linger_ms);
    } else if (config.role == runtime_config::Role::kReceiver) {
        receiver_app::SetupReceiver(config.receiver_filter_enabled, config.receiver_filter_ids.data(),
                                    config.receiver_filter_count);
    }
}
