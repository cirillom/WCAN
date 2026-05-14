#include <stdio.h>

#include "string.h"
#include "esp_log.h"

#include "can.hpp"

static const char *CAN_TAG = "CAN";
static bool s_twai_driver_installed = false;

static esp_err_t CanEnsureRunning(void)
{
    twai_status_info_t status = {};

    if (!s_twai_driver_installed) {
        ESP_LOGE(CAN_TAG, "TWAI driver not installed; call CanInit() before CanSend()");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t status_err = twai_get_status_info(&status);
    if (status_err != ESP_OK) {
        ESP_LOGE(CAN_TAG, "Failed to get TWAI status: %s", esp_err_to_name(status_err));
        return status_err;
    }

    ESP_LOGI(CAN_TAG, "State: %d", status.state);
    ESP_LOGI(CAN_TAG, "TX error counter: %ld", status.tx_error_counter);
    ESP_LOGI(CAN_TAG, "RX error counter: %ld", status.rx_error_counter);
    ESP_LOGI(CAN_TAG, "TX failed count: %ld", status.tx_failed_count);
    ESP_LOGI(CAN_TAG, "Arb lost count: %ld", status.arb_lost_count);
    ESP_LOGI(CAN_TAG, "Bus error count: %ld", status.bus_error_count);
    ESP_LOGI(CAN_TAG, "Msgs to TX: %ld", status.msgs_to_tx);
    ESP_LOGI(CAN_TAG, "Msgs to RX: %ld", status.msgs_to_rx);

    if (status.state == TWAI_STATE_RUNNING) {
        return ESP_OK;
    }

    if (status.state == TWAI_STATE_STOPPED) {
        esp_err_t start_err = twai_start();
        if (start_err == ESP_OK) {
            ESP_LOGW(CAN_TAG, "TWAI was stopped; restarted before transmit");
            return ESP_OK;
        }

        ESP_LOGE(CAN_TAG, "Failed to restart TWAI: %s", esp_err_to_name(start_err));
        return start_err;
    }

    if (status.state == TWAI_STATE_BUS_OFF) {
        esp_err_t recovery_err = twai_initiate_recovery();
        if (recovery_err != ESP_OK) {
            ESP_LOGE(CAN_TAG, "Failed to initiate TWAI recovery: %s", esp_err_to_name(recovery_err));
        } else {
            ESP_LOGW(CAN_TAG, "TWAI bus-off; recovery started");
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (status.state == TWAI_STATE_RECOVERING) {
        ESP_LOGW(CAN_TAG, "TWAI is recovering from bus-off");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(CAN_TAG, "TWAI not ready for transmit (state=%d)", status.state);
    return ESP_ERR_INVALID_STATE;
}

void CanInit(gpio_num_t tx_pin, gpio_num_t rx_pin)
{
    // esp_log_level_set(CAN_TAG, ESP_LOG_WARN);
    ESP_LOGI(CAN_TAG, "Initializing TWAI with TX pin %d, RX pin %d", tx_pin, rx_pin);

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (s_twai_driver_installed) {
        ESP_LOGW(CAN_TAG, "CanInit called again; TWAI already installed");
        return;
    }

    // Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        s_twai_driver_installed = true;
        ESP_LOGI(CAN_TAG, "Driver installed");
    } else {
        ESP_LOGE(CAN_TAG, "Failed to install driver");
        return;
    }

    // Start TWAI driver
    if (twai_start() == ESP_OK) {
        ESP_LOGI(CAN_TAG, "Driver started");
    } else {
        ESP_LOGE(CAN_TAG, "Failed to start driver");
        twai_driver_uninstall();
        s_twai_driver_installed = false;
        return;
    }
}

esp_err_t CanSend(uint32_t can_id, uint8_t payload_len, uint8_t *payload)
{
    static const char *TAG = "CAN-TX";

    if (payload_len > 8) {
        ESP_LOGE(TAG, "Invalid payload length %u (max 8)", payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    if (payload_len > 0 && payload == NULL) {
        ESP_LOGE(TAG, "Payload is NULL with non-zero length");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t state_err = CanEnsureRunning();
    if (state_err != ESP_OK) {
        ESP_LOGE(TAG, "Skipping CAN send because TWAI is not in running state");
        return state_err;
    }

    twai_message_t message = {};
    message.identifier = can_id;
    message.data_length_code = payload_len;
    message.extd = (can_id > 0x7FF);
    if (payload_len > 0) {
        memcpy(message.data, payload, payload_len);
    }

    esp_err_t err = twai_transmit(&message, pdMS_TO_TICKS(1000));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Message queued for transmission");
    } else {
        ESP_LOGE(TAG, "Failed to queue message for transmission: %s", esp_err_to_name(err));
    }

    return err;
}

void CanReceiveTask(void *pvParameter)
{
    static const char *TAG = "CAN-RX";
    twai_message_t message;

    ESP_LOGI(TAG, "CAN receive task started");

    while (1) {
        esp_err_t err = twai_receive(&message, pdMS_TO_TICKS(1000));
        if (err == ESP_OK) {
            uint32_t value = 0;
            memcpy(&value, message.data,
                   message.data_length_code > sizeof(value) ? sizeof(value) : message.data_length_code);
            ESP_LOGI(TAG, "[%08lx] %lu", (unsigned long int)message.identifier, (unsigned long)value);
        } else if (err == ESP_ERR_TIMEOUT) {
            // No message received, continue
            ESP_LOGI(TAG, "No message received within timeout");
        } else {
            ESP_LOGE(TAG, "twai_receive failed: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

// task to validate CAN transmission and reception without WCAN filtering, just to test the CAN communication first
void CanTxTestTask(void *pvParameter)
{
    static const char *TAG = "CanTestTask";
    uint32_t counter = 0;

    ESP_LOGI(TAG, "CanTestTask started");

    while (1) {
        esp_err_t err = CanSend(0x100, sizeof(counter), (uint8_t *)&counter);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent CAN message with counter=%lu", (unsigned long)counter);
        } else {
            ESP_LOGW(TAG, "CAN send skipped/failed for counter=%lu: %s", (unsigned long)counter, esp_err_to_name(err));
        }
        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}