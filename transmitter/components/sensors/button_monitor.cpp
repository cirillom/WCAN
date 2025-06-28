#include "button_monitor.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

esp_log_level_t log_level = ESP_LOG_WARN; // Default log level for this component

static TimerHandle_t confirmation_timer = NULL;
static gpio_num_t monitored_pin;
static int can_id = 0;

static void confirmation_timer_callback(TimerHandle_t xTimer) {
    static const char *TAG = "BUTTON_MONITOR";
    esp_log_level_set(TAG, log_level);

    ESP_LOGD(TAG, "Confirmation timer callback triggered");

    if (gpio_get_level(monitored_pin) == 1) {
        data_packet_t send_data;
        send_data.can_id = can_id;
        send_data.payload = NULL;
        send_data.payload_len = 0;

        int message = 0xFF;
        send_data.payload_len = sizeof(message);
        send_data.payload = (uint8_t *)malloc(send_data.payload_len);
        if (send_data.payload == NULL) {
            ESP_LOGE(TAG, "Malloc payload fail");
            return;
        }
        memcpy(send_data.payload, &message, send_data.payload_len);

        if (xQueueSend(send_queue, &send_data, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Send queue fail");
            free(send_data.payload);
        } else {
            ESP_LOGD(TAG, "Data sent to queue successfully");
        }
    }
}

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTimerResetFromISR(confirmation_timer, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t button_monitor_init(int can_id, gpio_num_t gpio_pin, uint32_t confirmation_time_ms) {
    static const char *TAG = "BUTTON_MONITOR";
    esp_log_level_set(TAG, log_level); // Set log level for this component

    monitored_pin = gpio_pin;

    confirmation_timer = xTimerCreate(
        "confirmation_tmr",
        pdMS_TO_TICKS(confirmation_time_ms),
        pdFALSE,
        (void*)0,
        confirmation_timer_callback
    );

    if (confirmation_timer == NULL) {
        ESP_LOGE(TAG, "Create confirmation timer fail");
        return ESP_FAIL;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .pin_bit_mask = (1ULL << gpio_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(gpio_pin, gpio_isr_handler, (void*) gpio_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO ISR handler add failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
