#include <stdio.h>
#include <nvs_flash.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "string.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "driver/twai.h"

#include "wcan_communication.h"
#include "wcan_utils.h"

uint16_t allowed_ids[3] = {0x123, 0x456, 0x789};
#define ALLOWED_IDS_SIZE (sizeof(allowed_ids) / sizeof(allowed_ids[0]))

#define MAX_UINT32 0xFFFFFFFF

static void ReadDataTask(void *pvParameter){
    static const char *TAG = "READ";
    ESP_LOGI(TAG, "Read data task started");
    uint32_t val = 0;

    while(1) {
        data_packet_t send_data;
        send_data.can_id = 0x123;
        send_data.payload = NULL;
        send_data.payload_len = 0;

        send_data.payload_len = sizeof(val);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);
        if (send_data.payload == NULL) {
            ESP_LOGE(TAG, "Malloc payload fail");
            break;
        }
        memcpy(send_data.payload, &val, send_data.payload_len);
        ESP_LOGI(TAG, "[%04x] %lu", send_data.can_id, val);

        ESP_LOGV(TAG, "send_data.payload: %p\n", (void*)send_data.payload);
        if (xQueueSend(send_queue, &send_data, ESPNOW_MAXDELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Data read successfully");
        } else {
            ESP_LOGW(TAG, "Send send queue fail");
            free(send_data.payload);
        }

        val = (val + 1) % MAX_UINT32;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void RecvCallback(data_packet_t data)
{
    static const char *TAG = "USER-RECV";
    switch (data.can_id)
    {
        case 0x123:{
            uint32_t uint_data = *(uint32_t *)(data.payload);
            if (uint_data % 5 != 0) break;
            
            ESP_LOGI(TAG, "[%04x] %lu", data.can_id, uint_data);
            // Configure message to transmit
            twai_message_t message = {
                .identifier = 0x123,
                .data_length_code = data.payload_len,
            };

            memcpy(message.data, data.payload, data.payload_len);

            twai_status_info_t status_info;
            if (twai_get_status_info(&status_info) == ESP_OK) {
                ESP_LOGI(TAG, "TWAI Status: state=%d, msgs_to_tx=%ld, msgs_to_rx=%ld, tx_err_cnt=%ld, rx_err_cnt=%ld, arb_lost_cnt=%ld, bus_err_cnt=%ld",
                        status_info.state,
                        status_info.msgs_to_tx,
                        status_info.msgs_to_rx,
                        status_info.tx_error_counter,
                        status_info.rx_error_counter,
                        status_info.arb_lost_count,
                        status_info.bus_error_count);
                if (status_info.state == TWAI_STATE_BUS_OFF) {
                    ESP_LOGE(TAG, "TWAI controller is BUS OFF. Attempting recovery...");
                    twai_initiate_recovery(); // Or twai_start() again after twai_stop() depending on recovery strategy
                }
            }

            // Queue message for transmission
            esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(1000));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Message queued for transmission\n");
            } else {
                ESP_LOGE(TAG, "Failed to queue message for transmission: %s\n", esp_err_to_name(err));
            }
            break;
        }
        default:
            ESP_LOGE(TAG, "[%04x] Unknown", data.can_id);
            PrintCharPacket(data.payload, data.payload_len);
            break;
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
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_25KBITS();
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