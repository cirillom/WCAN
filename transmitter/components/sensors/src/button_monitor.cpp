#include "button_monitor.h"

#include "string.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

esp_log_level_t log_level = ESP_LOG_INFO; // Default log level for this component

static TimerHandle_t confirmation_timer = NULL;
static TimerHandle_t restart_isr_timer = NULL;
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

        // Remove the ISR handler to prevent further interrupts
        esp_err_t err = gpio_isr_handler_remove(monitored_pin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO ISR handler remove failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "GPIO ISR handler removed successfully");
        }
        // Restart the ISR handler after a delay
        if (xTimerReset(restart_isr_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to reset restart ISR timer");
        } else {
            ESP_LOGD(TAG, "Restart ISR timer reset successfully");
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

static void restart_isr(TimerHandle_t xTimer) {
    static const char *TAG = "BUTTON_MONITOR";
    esp_log_level_set(TAG, log_level);

    esp_err_t err = gpio_isr_handler_add(monitored_pin, gpio_isr_handler, (void*) monitored_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO ISR handler add failed: %s", esp_err_to_name(err));
    }

}

esp_err_t button_monitor_init(int can_id_, gpio_num_t gpio_pin, uint32_t confirmation_time_ms) {
    static const char *TAG = "BUTTON_MONITOR";
    esp_log_level_set(TAG, log_level); // Set log level for this component

    ESP_LOGI(TAG, "Initializing button monitor on GPIO %d with confirmation time %ld ms", gpio_pin, confirmation_time_ms);
    monitored_pin = gpio_pin;
    can_id = can_id_;

    TickType_t period_in_ticks = pdMS_TO_TICKS(confirmation_time_ms);

    // --- ADD THIS CHECK ---
    if (period_in_ticks <= 0) {
        period_in_ticks = 1; 
        ESP_LOGW(TAG, "confirmation_time_ms is less than one RTOS tick. Defaulting to 1 tick.");
    }

    confirmation_timer = xTimerCreate(
        "confirmation_tmr",
        period_in_ticks, // Use the safe, checked value
        pdFALSE,
        (void*)0,
        confirmation_timer_callback
    );

    if (confirmation_timer == NULL) {
        ESP_LOGE(TAG, "Create confirmation timer fail");
        return ESP_FAIL;
    }

    restart_isr_timer = xTimerCreate(
        "restart_isr_tmr",
        pdMS_TO_TICKS(1000), // 1 second
        pdFALSE,
        (void*)0,
        restart_isr
    );


    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
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

    xTimerReset(restart_isr_timer, 0);

    return ESP_OK;
}
