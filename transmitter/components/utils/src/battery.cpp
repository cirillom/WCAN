// battery.cpp

#include "battery.hpp"
#include "esp_log.h"
#include "esp_sleep.h"

// Define a tag for logging
static const char *TAG = "BATTERY";

battery_t::battery_t(gpio_num_t power_on_pin, gpio_num_t charging_status_pin, adc_channel_t adc_channel)
    : power_on_pin_(power_on_pin),
      charging_status_pin_(charging_status_pin),
      adc_channel_(adc_channel) {
    
    esp_log_level_set(TAG, ESP_LOG_INFO); // Set log level for this component

    initialize_gpio();
    initialize_adc();

    ESP_LOGI(TAG, "Battery component initialized.");
}

battery_t::~battery_t() {
    // Clean up resources
    if (monitoring_task_handle_) {
        vTaskDelete(monitoring_task_handle_);
    }
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc_handle_));
    if (adc_cali_handle_) {
        // This function does not exist in the current API, cleanup is handled by del_unit
        // adc_cali_delete_scheme_line_fitting(adc_cali_handle_);
    }
    ESP_LOGI(TAG, "Battery component de-initialized.");
}


void battery_t::initialize_gpio() {
    // Configure the pin that keeps the device powered on
    gpio_reset_pin(power_on_pin_);
    gpio_set_direction(power_on_pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(power_on_pin_, 1);
    ESP_LOGI(TAG, "Power ON pin (GPIO %d) set to HIGH.", power_on_pin_);

    // Configure the charging status input pin
    gpio_reset_pin(charging_status_pin_);
    gpio_set_direction(charging_status_pin_, GPIO_MODE_INPUT);
    gpio_set_pull_mode(charging_status_pin_, GPIO_PULLDOWN_ONLY);
    ESP_LOGI(TAG, "Charging status pin (GPIO %d) configured as input with pulldown.", charging_status_pin_);
}

void battery_t::initialize_adc() {
    //-------------ADC1 Init---------------//
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

    //-------------ADC1 Channel Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12, // Corresponds to a full-scale voltage of approx. 3.3V
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &config));

}

esp_err_t battery_t::get_voltage_mv(int* out_voltage_mv) {
    if (!out_voltage_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    int adc_raw;
    int voltage_calibrated;

    esp_err_t read_err = adc_oneshot_read(adc_handle_, adc_channel_, &adc_raw);
    if (read_err != ESP_OK) {
        return read_err;
    }

    if (adc_cali_handle_) {
        esp_err_t cali_err = adc_cali_raw_to_voltage(adc_cali_handle_, adc_raw, &voltage_calibrated);
        if (cali_err != ESP_OK) {
            return cali_err;
        }
    } else {
        // Fallback if calibration is not available (less accurate)
        // This is a rough estimation. For production, ensure calibration works.
        voltage_calibrated = (adc_raw * 3300) / 4095;
    }

    // The user's original code implies a 1:1 voltage divider (e.g., two 100k resistors).
    // This means the ADC voltage is half the battery voltage.
    // Adjust this multiplier if your voltage divider is different.
    *out_voltage_mv = voltage_calibrated * 2;

    return ESP_OK;
}


void battery_t::enter_deep_sleep_if_not_charging() {
    int pin_state = gpio_get_level(charging_status_pin_);

    if (pin_state == 0) {
        ESP_LOGW(TAG, "Device is not charging. Entering deep sleep.");
        ESP_LOGW(TAG, "Will wake up when GPIO %d goes HIGH.", charging_status_pin_);
        
        // Flush logs before sleeping
        esp_log_level_set("*", ESP_LOG_NONE); // Optional: disable logging to speed up sleep
        
        ESP_ERROR_CHECK(gpio_wakeup_enable(charging_status_pin_, GPIO_INTR_HIGH_LEVEL));
        ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
        
        esp_deep_sleep_start(); // This function does not return
    } else {
        ESP_LOGI(TAG, "Device is charging, not entering sleep.");
    }
}

void battery_t::start_monitoring_task(uint32_t interval_ms) {
    this->monitoring_interval_ms_ = interval_ms;
    // xTaskCreate(task_function, "task_name", stack_size, task_parameters, priority, task_handle)
    xTaskCreate(
        this->monitoring_task_trampoline,
        "battery_monitor_task",
        2048,
        this, // Pass the current object instance as an argument
        5,
        &monitoring_task_handle_
    );
}

// This static "trampoline" function allows us to call a C++ member function from a FreeRTOS task.
void battery_t::monitoring_task_trampoline(void* arg) {
    battery_t* instance = static_cast<battery_t*>(arg);
    instance->monitoring_task();
}

void battery_t::monitoring_task() {
    ESP_LOGI(TAG, "Battery monitoring task started. Reading every %lu ms.", monitoring_interval_ms_);
    while (1) {
        int voltage_mv = 0;
        esp_err_t err = get_voltage_mv(&voltage_mv);

        if (err == ESP_OK) {
            // Replicate the user's desired output format
            ESP_LOGI(TAG, "--------------");
            ESP_LOGI(TAG, "BAT millivolts value = %d mV", voltage_mv);
            // You can easily calculate and print a battery percentage here if you have a voltage curve
            // Example:
            // float percentage = ((float)voltage_mv - 3200.0) / (4200.0 - 3200.0) * 100.0;
            // if (percentage < 0) percentage = 0;
            // if (percentage > 100) percentage = 100;
            // ESP_LOGI(TAG, "BAT percentage = %.1f %%", percentage);
        } else {
            ESP_LOGE(TAG, "Failed to read battery voltage, error: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(this->monitoring_interval_ms_));
    }
}