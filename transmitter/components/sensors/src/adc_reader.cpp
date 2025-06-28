#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wcan_communication.h"
#include "adc_reader.h"

static const char *TAG = "ADC_READER";

// ADC handle, shared across all reader tasks.
static adc_oneshot_unit_handle_t adc1_handle = NULL;

// Structure to pass parameters to each new task.
typedef struct {
    adc_channel_t channel;
    uint32_t can_id;
    uint32_t delay_ms;
} adc_reader_task_params_t;

static void adc_reader_task(void *pvParameter) {
    // Retrieve task parameters and then free the allocated memory
    adc_reader_task_params_t *params = (adc_reader_task_params_t *)pvParameter;
    const adc_channel_t channel = params->channel;
    const uint32_t can_id = params->can_id;
    const uint32_t delay_ms = params->delay_ms;
    free(params);

    ESP_LOGI(TAG, "Task started for ADC channel %d, CAN ID 0x%04lX, Freq %ldms", channel, can_id, delay_ms);

    while (1) {
        int adc_raw_val = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle, channel, &adc_raw_val);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed on channel %d: %s", channel, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }

        data_packet_t send_data;
        send_data.can_id = can_id;
        send_data.payload_len = sizeof(adc_raw_val);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);

        if (send_data.payload == NULL) {
            ESP_LOGE(TAG, "Payload malloc failed for CAN ID 0x%04lX", can_id);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }
        
        memcpy(send_data.payload, &adc_raw_val, send_data.payload_len);

        if (xQueueSend(send_queue, &send_data, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Output queue send fail for CAN ID 0x%04lX", can_id);
            free(send_data.payload);
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    // This part is unreachable in this implementation but good practice
    vTaskDelete(NULL);
}


esp_err_t adc_reader_start_task(adc_channel_t channel, uint32_t can_id, uint32_t frequency_hz) {
    ESP_LOGI(TAG, "Starting ADC reader task for channel %d, CAN ID 0x%04lX, Frequency %ldHz", channel, can_id, frequency_hz);

    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE, // ULP mode is not used here
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ret = adc_oneshot_config_channel(adc1_handle, channel, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel %d config failed: %s", channel, esp_err_to_name(ret));
        return ret;
    }

    adc_reader_task_params_t *task_params = (adc_reader_task_params_t*)malloc(sizeof(adc_reader_task_params_t));
    if (task_params == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for task parameters");
        return ESP_ERR_NO_MEM;
    }

    task_params->channel = channel;
    task_params->can_id = can_id;
    task_params->delay_ms = (frequency_hz > 0) ? (1000 / frequency_hz) : 1000;
    
    char task_name[configMAX_TASK_NAME_LEN];
    snprintf(task_name, sizeof(task_name), "adc_reader_%d", channel);

    BaseType_t task_created = xTaskCreate(adc_reader_task, task_name, 2048, task_params, 5, NULL);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task for ADC channel %d", channel);
        free(task_params); // Free memory if task creation fails
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
