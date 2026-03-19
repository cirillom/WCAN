#include <nvs_flash.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

#include "wcan.h"
#include "wcan_utils.h"
#include "can.h"

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

// Placeholder task: increments a counter and sends it over WCAN to test data transmission
static void ReadDataTask(void *pvParameter)
{
    static const char *TAG = "ReadDataTask";
    uint32_t counter = 0;

    ESP_LOGI(TAG, "ReadDataTask started");

    while (1)
    {
        data_packet_t send_data;
        send_data.can_id = 0x100;
        send_data.payload_len = sizeof(counter);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);

        if (send_data.payload == NULL)
        {
            ESP_LOGE(TAG, "Payload malloc failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        memcpy(send_data.payload, &counter, send_data.payload_len);

        if (xQueueSend(send_queue, &send_data, pdMS_TO_TICKS(10)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Send queue full, dropping counter=%lu", (unsigned long)counter);
            free(send_data.payload);
        }
        else
        {
            ESP_LOGI(TAG, "Sent counter=%lu", (unsigned long)counter);
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    static const char *TAG = "MAIN";

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // #region Development, instead of making a build for sensor and receiver, just separate the software flow using MAC
    char *receiver = "34:5f:45:ab:82:0c";
    char *sensor = "f0:f5:bd:2c:16:b8";

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char *mac_str = (char *)malloc(18);
    snprintf(mac_str, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "MAC address string: %s", mac_str);
    // #endregion

    WiFiInit();
    ESP_LOGI(TAG, "WiFi initialized");
    uint16_t allowed_ids[] = {};
    size_t allowed_ids_size = 0;

    if (strcmp(mac_str, receiver) == 0)
    {
        ESP_LOGI(TAG, "This is the receiver");

        CanInit();
        size_t car_allowed_recv_ids_size = 16;
        static uint16_t car_allowed_recv_ids[] = {
            0x550, 0x551, 0x552, 0x553, 0x554, 0x555, 0x556, 0x557,
            0x558, 0x559, 0x55A, 0x55B, 0x55C, 0x55D, 0x55E, 0x55F};

        WCAN_Init(true, car_allowed_recv_ids, car_allowed_recv_ids_size);
    }
    else if (strcmp(mac_str, sensor) == 0)
    {
        ESP_LOGI(TAG, "This is the sensor");
        WCAN_Init(false, allowed_ids, allowed_ids_size);

        xTaskCreate(ReadDataTask, "ReadDataTask", 4096, NULL, 5, NULL);
    }
    else
    {
        ESP_LOGE(TAG, "Unknown MAC address: %s", mac_str);
        free(mac_str);
        return;
    }
}

// read data task, just a place holder for a generic task that will increment a number one by one and then send it, this will be used to check data transmmission
