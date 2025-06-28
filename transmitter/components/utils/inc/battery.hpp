// battery.hpp

#ifndef BATTERY_HPP
#define BATTERY_HPP

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class battery_t {
public:
    /**
     * @brief Construct a new battery_t object
     *
     * @param power_on_pin The GPIO pin used to keep the device power circuitry enabled.
     * @param charging_status_pin The GPIO pin used to read the charging status.
     * @param adc_channel The ADC channel connected to the battery voltage sense pin.
     */
    battery_t(gpio_num_t power_on_pin, gpio_num_t charging_status_pin, adc_channel_t adc_channel);

    /**
     * @brief Destructor
     */
    ~battery_t();

    /**
     * @brief Starts the background task to monitor and print battery voltage.
     *
     * @param interval_ms The interval in milliseconds between each measurement.
     */
    void start_monitoring_task(uint32_t interval_ms = 2000);

    /**
     * @brief Checks the charging status and enters deep sleep if not charging.
     *
     * The device will configure itself to wake up when the charging status pin goes HIGH.
     * This function will not return if the device goes to sleep.
     */
    void enter_deep_sleep_if_not_charging();

    /**
     * @brief Performs a single, synchronous read of the battery voltage.
     *
     * @param[out] out_voltage_mv Pointer to store the calculated battery voltage in millivolts.
     * @return esp_err_t ESP_OK on success, or an error code on failure.
     */
    esp_err_t get_voltage_mv(int* out_voltage_mv);


private:
    // Member variables for configuration
    gpio_num_t power_on_pin_;
    gpio_num_t charging_status_pin_;
    adc_channel_t adc_channel_;
    uint32_t monitoring_interval_ms_;

    // Handles for ESP-IDF components
    adc_oneshot_unit_handle_t adc_handle_;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    TaskHandle_t monitoring_task_handle_ = nullptr;
    
    // Private methods
    void initialize_gpio();
    void initialize_adc();
    static void monitoring_task_trampoline(void* arg);
    void monitoring_task();
};

#endif // BATTERY_HPP