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

uint16_t allowed_ids[16] = {0x50, 0x551, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
#define ALLOWED_IDS_SIZE (sizeof(allowed_ids) / sizeof(allowed_ids[0]))

#define OUTPUT_HIGH_PIN     GPIO_NUM_23
#define CHARGING_CTRL_PIN   GPIO_NUM_22

#define MAX_UINT32 0xFFFFFFFF

#include "esp_adc/adc_oneshot.h"
#define ADC_CHANNEL ADC_CHANNEL_4  // GPIO4 corresponds to ADC1_CHANNEL_4

static void ReadDataTask(void *pvParameter) {
    static const char *TAG = "READ_TASK";

    //------------- 1. ADC One-Shot Unit Initialization (do this once) ----------------
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE, // ULP mode is not used here
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    //------------- 2. ADC One-Shot Channel Configuration -------------------------
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config));

    ESP_LOGI(TAG, "Read data task started using Wi-Fi safe one-shot driver.");
    
    while (1) {
        int val = 0;

        //------------- 3. ADC Read (Wi-Fi Safe) ------------------------------------
        esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL, &val);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "[%04x] %i", 0x551, val);
        } else {
            ESP_LOGE(TAG, "ADC one-shot read failed with error: %s", esp_err_to_name(ret));
        }

        data_packet_t send_data;
        send_data.can_id = 0x551;
        send_data.payload = NULL;
        send_data.payload_len = 0;

        send_data.payload_len = sizeof(val);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);
        if (send_data.payload == NULL) {
            ESP_LOGE(TAG, "Malloc payload fail");
            // Consider a small delay here to prevent a tight loop of failures
            vTaskDelay(pdMS_TO_TICKS(1000)); 
            continue; // Use continue instead of break to keep the task alive
        }
        memcpy(send_data.payload, &val, send_data.payload_len);

        if (xQueueSend(send_queue, &send_data, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Send queue fail");
            // IMPORTANT: Free the memory if the send fails, otherwise it's lost
            free(send_data.payload);
        } else {
            // The receiving task is now responsible for freeing send_data.payload
            ESP_LOGD(TAG, "Data sent to queue successfully");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // This part is unlikely to be reached in this example, but good practice
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
    vTaskDelete(NULL);
}

void RecvCallback(data_packet_t data)
{
    static const char *TAG = "USER-RECV";
    switch (data.can_id)
    {
        case 0x551:{
            uint32_t uint_data = *(uint32_t *)(data.payload);
            ESP_LOGI(TAG, "Strain Gauge [%04x]: %lu", data.can_id, uint_data);
            break;
        }
        default:{
            ESP_LOGE(TAG, "[%04x] Unknown", data.can_id);
            PrintCharPacket(data.payload, data.payload_len);
            break;
        }
    }

    twai_message_t message = {
        .identifier = (uint32_t)data.can_id,
        .data_length_code = data.payload_len,
    };
    memcpy(message.data, data.payload, data.payload_len);

    esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(1000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Message queued for transmission\n");
    } else {
        ESP_LOGE(TAG, "Failed to queue message for transmission: %s\n", esp_err_to_name(err));
        twai_stop();
        twai_start();
    }
}

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

static void CanInit(){
    static const char *TAG = "CAN";
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_22, GPIO_NUM_21, TWAI_MODE_NO_ACK);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "Driver installed\n");
    } else {
        ESP_LOGE(TAG, "Failed to install driver\n");
        return;
    }

    // Start TWAI driver
    if (twai_start() == ESP_OK) {
        ESP_LOGI(TAG, "Driver started\n");
    } else {
        ESP_LOGE(TAG, "Failed to start driver\n");
        return;
    }
}

extern "C" void app_main(void){
    static const char *TAG = "MAIN";
    /*
    gpio_reset_pin(OUTPUT_HIGH_PIN);
    gpio_set_direction(OUTPUT_HIGH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(OUTPUT_HIGH_PIN, 1);

    gpio_reset_pin(CHARGING_CTRL_PIN);
    gpio_set_direction(CHARGING_CTRL_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CHARGING_CTRL_PIN, GPIO_PULLDOWN_ONLY);

    int pin_state = gpio_get_level(CHARGING_CTRL_PIN);

    if (pin_state == 0) {
        ESP_ERROR_CHECK(gpio_wakeup_enable(CHARGING_CTRL_PIN, GPIO_INTR_HIGH_LEVEL));
        ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
        ESP_LOGI(TAG, "Going to sleep now...");
        esp_deep_sleep_start();
    }
    */
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    WiFiInit();
    ESP_LOGI(TAG, "WiFi initialized");
    CanInit();
    ESP_LOGI(TAG, "CAN initialized");
    WCAN_Init(true, allowed_ids, ALLOWED_IDS_SIZE);
    ESP_LOGI(TAG, "Setup completed");

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    ESP_LOGI(TAG, "MAC address: %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    //xTaskCreate(ReadDataTask, "ReadDataTask", 4096, NULL, 5, NULL);
}