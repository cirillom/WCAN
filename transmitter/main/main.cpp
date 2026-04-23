#include <nvs_flash.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

#include "wcan.h"
#include "wcan_utils.h"

// Compile-time role validation
#if !defined(ROLE_SENSOR) && !defined(ROLE_RECEIVER) && !defined(ROLE_IDLE)
#error "Build must define ROLE=SENSOR or ROLE=RECEIVER or ROLE=IDLE via CMake (-DROLE=SENSOR or -DROLE=RECEIVER or -DROLE=IDLE)"
#endif

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

#ifdef ROLE_SENSOR
// Sensor task: increments a counter and sends it over WCAN
// CAN ID is derived from the board's MAC address for unique identification
static void ReadDataTask(void *pvParameter)
{
    int frequency_hertz = 10; // Send data every 1 second

    static const char *TAG = "ReadDataTask";
    uint32_t can_id = (uint32_t)(uintptr_t)pvParameter;
    uint32_t counter = 0;

    ESP_LOGI(TAG, "ReadDataTask started with CAN ID 0x%04lx", (unsigned long)can_id);

    while (1)
    {
        data_packet_t send_data;
        send_data.can_id = can_id;
        send_data.tick_count = xTaskGetTickCount();
        send_data.data_count = 1;
        send_data.payload_len = sizeof(counter);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);

        if (send_data.payload == NULL) 
        {
            ESP_LOGE(TAG, "Payload malloc failed");
        } 
        else 
        {
            memcpy(send_data.payload, &counter, send_data.payload_len);
    
            if (xQueueSend(send_queue, &send_data, pdMS_TO_TICKS(10)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Send queue full, dropping counter=%lu", (unsigned long)counter);
                free(send_data.payload);
            }
            else
            {
                ESP_LOGI(TAG, "%lu", (unsigned long)counter);
            }
            counter++;
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / frequency_hertz));
    }

    vTaskDelete(NULL);
}
#endif // ROLE_SENSOR

#ifdef ROLE_RECEIVER
// Receiver callback: logs any incoming CAN ID and payload
void RecvCallback(data_packet_t data)
{
    static const char *TAG = "RecvCallback";

    if (data.payload_len >= sizeof(uint32_t))
    {
        uint32_t counter_value = *(uint32_t *)(data.payload);
        ESP_LOGI(TAG, "[%04lx] %lu",
                 (unsigned long)data.can_id,
                 (unsigned long)counter_value);
    }
    else
    {
        ESP_LOGI(TAG, "[%04lx] len=%u payload_too_short",
                 (unsigned long)data.can_id,
                 data.payload_len);
    }
}
#endif // ROLE_RECEIVER

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

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    WiFiInit();
    ESP_LOGI(TAG, "WiFi initialized");

#ifdef ROLE_SENSOR
    // Derive a unique CAN ID from the last 2 bytes of the MAC address
    uint32_t can_id = ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    ESP_LOGI(TAG, "SENSOR mode — CAN ID: 0x%04lx", (unsigned long)can_id);

    // Sensor should not receive any messages, so we filter everything out
    WCAN_Init(true, NULL, 0);

    xTaskCreate(ReadDataTask, "ReadDataTask", 4096, (void *)(uintptr_t)can_id, 5, NULL);

#elif defined(ROLE_RECEIVER)
    ESP_LOGI(TAG, "RECEIVER mode — accepting all CAN IDs");

    // filter=false means accept everything
    WCAN_Init(false, NULL, 0);

#elif defined(ROLE_IDLE)
    ESP_LOGI(TAG, "IDLE mode — doing nothing");
    // No WiFi, no WCAN — board is completely silent
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

#endif
}