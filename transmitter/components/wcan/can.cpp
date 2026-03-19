#include <stdio.h>

#include "string.h"
#include "esp_log.h"

#include "can.h"

void CanInit(gpio_num_t tx_pin, gpio_num_t rx_pin)
{
    static const char *TAG = "CAN";
    esp_log_level_set(TAG, ESP_LOG_WARN);

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NO_ACK);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK)
    {
        ESP_LOGI(TAG, "Driver installed\n");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to install driver\n");
        return;
    }

    // Start TWAI driver
    if (twai_start() == ESP_OK)
    {
        ESP_LOGI(TAG, "Driver started\n");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start driver\n");
        return;
    }
}

void CanSend(uint32_t can_id, uint8_t payload_len, uint8_t *payload)
{
    static const char *TAG = "CAN-TX";

    twai_message_t message = {
        .identifier = can_id,
        .data_length_code = payload_len,
    };
    memcpy(message.data, payload, payload_len);

    esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(1000));
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Message queued for transmission");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to queue message for transmission: %s", esp_err_to_name(err));
        twai_stop();
        twai_start();
    }
}

void CanReceiveTask(void *pvParameter)
{
    static const char *TAG = "CAN-RX";
    twai_message_t message;

    ESP_LOGI(TAG, "CAN receive task started");

    while (1)
    {
        esp_err_t err = twai_receive(&message, pdMS_TO_TICKS(1000));
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Received ID: 0x%08lx DLC: %d",
                     (unsigned long)message.identifier, message.data_length_code);

            printf("  Data:");
            for (int i = 0; i < message.data_length_code; i++)
            {
                printf(" %02X", message.data[i]);
            }
            printf("\n");
        }
        else if (err == ESP_ERR_TIMEOUT)
        {
            // No message received, continue
        }
        else
        {
            ESP_LOGE(TAG, "twai_receive failed: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}
