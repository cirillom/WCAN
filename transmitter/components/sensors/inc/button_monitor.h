#ifndef BUTTON_MONITOR_H
#define BUTTON_MONITOR_H

#include "driver/gpio.h"
#include "esp_err.h"

#include "wcan_communication.h"

esp_err_t button_monitor_init(int can_id, gpio_num_t gpio_pin, uint32_t confirmation_time_ms);

#endif // BUTTON_MONITOR_H