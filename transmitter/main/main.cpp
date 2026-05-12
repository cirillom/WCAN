#include <cstdint>

#include <nvs_flash.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wcan.hpp"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#if defined(ROLE_SENSOR)
#include "sensor_app.hpp"
#elif defined(ROLE_RECEIVER)
#include "receiver_app.hpp"
#endif

#if !defined(ROLE_SENSOR) && !defined(ROLE_RECEIVER) && !defined(ROLE_IDLE)
#error "Build must define ROLE=SENSOR, ROLE=RECEIVER, or ROLE=IDLE"
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
#ifdef ROLE_IDLE
    return;
#endif

#ifdef MEASURE_INSTR
    ESP_LOGI("BOOT_TS", "us=%lld", esp_timer_get_time());
#endif

    init_nvs();
    init_wifi();
    log_mac();

#ifdef ROLE_SENSOR
#ifdef SENSOR_MULTIPLE_CAN_IDS
    sensor_app::SetupMultipleCanIdSensor();
#else
    sensor_app::SetupSingleCanIdSensor();
#endif
#elif defined(ROLE_RECEIVER)
    receiver_app::SetupReceiver();
#endif
}
