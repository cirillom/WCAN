#include <stdio.h>
#include <nvs_flash.h>
#include <stdbool.h>

#include "string.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/twai.h"

#include "wcan_communication.h"
#include "wcan_utils.h"

#include "car.h"
#include "adc_reader.h"
#include "button_monitor.h"

static void WiFiInit(void)
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

extern "C" void app_main(void){
    static const char *TAG = "MAIN";
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //mac for the car board
    char *car_MAC = "f0:f5:bd:2c:19:40";
    char *strain_gauge_FL_MAC = "f0:f5:bd:2c:16:b8";
    char *strain_gauge_FR_MAC = "00:00:00:00:00:02";
    char *strain_gauge_RL_MAC = "00:00:00:00:00:03";
    char *strain_gauge_RR_MAC = "00:00:00:00:00:04";
    char *display_beacon_MAC = "f0:f5:bd:2c:18:a8";
    char *simple_beacon_MAC = "00:00:00:00:00:06";


    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char *mac_str = (char *)malloc(18);
    snprintf(mac_str, 18, "%02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "MAC address string: %s", mac_str);

    WiFiInit();
    ESP_LOGI(TAG, "WiFi initialized");
    uint16_t allowed_ids[] = {};
    size_t allowed_ids_size = 0;

    if(strcmp(mac_str, car_MAC) == 0){
        ESP_LOGI(TAG, "This is the car board");

        CanInit();
        size_t car_allowed_recv_ids_size = 16;
        static uint16_t car_allowed_recv_ids[] = {0x550, 0x551, 0x552, 0x553, 0x554, 0x555, 0x556, 0x557, 0x558, 0x559, 0x55A, 0x55B, 0x55C, 0x55D, 0x55E, 0x55F};

        WCAN_Init(true, car_allowed_recv_ids, car_allowed_recv_ids_size);

    } else if (strcmp(mac_str, strain_gauge_FL_MAC) == 0){
        ESP_LOGI(TAG, "This is the front left strain gauge");
        WCAN_Init(false, allowed_ids, allowed_ids_size);

        adc_reader_start_task(ADC_CHANNEL_4, 0x551, 10);

    } else if (strcmp(mac_str, strain_gauge_FR_MAC) == 0){
        ESP_LOGI(TAG, "This is the front right strain gauge");
    } else if (strcmp(mac_str, strain_gauge_RL_MAC) == 0){
        ESP_LOGI(TAG, "This is the rear left strain gauge");
    } else if (strcmp(mac_str, strain_gauge_RR_MAC) == 0){
        ESP_LOGI(TAG, "This is the rear right strain gauge");
    } else if (strcmp(mac_str, display_beacon_MAC) == 0){
        ESP_LOGI(TAG, "This is the display beacon");
        WCAN_Init(false, allowed_ids, allowed_ids_size);
        button_monitor_init(0x55A, GPIO_NUM_17, 20);

    } else if (strcmp(mac_str, simple_beacon_MAC) == 0){
        ESP_LOGI(TAG, "This is the simple beacon");
    } else {
        ESP_LOGE(TAG, "Unknown MAC address: %s", mac_str);
        free(mac_str);
        return;
    }

    //xTaskCreate(ReadDataTask, "ReadDataTask", 4096, NULL, 5, NULL);
}