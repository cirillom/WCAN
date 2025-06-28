#include <stdio.h>

#include "string.h"
#include "esp_log.h"

#include "car.h"
#include "wcan_communication.h"

void CanInit(){
    static const char *TAG = "CAN";
    esp_log_level_set(TAG, ESP_LOG_WARN);

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
            //PrintCharPacket(data.payload, data.payload_len);
            break;
        }
    }

    twai_message_t message = {
        .identifier = (uint32_t)data.can_id,
        .data_length_code = data.payload_len,
    };
    memcpy(message.data, data.payload, data.payload_len);

    return;

    esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(1000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Message queued for transmission\n");
    } else {
        ESP_LOGE(TAG, "Failed to queue message for transmission: %s\n", esp_err_to_name(err));
        twai_stop();
        twai_start();
    }
}
