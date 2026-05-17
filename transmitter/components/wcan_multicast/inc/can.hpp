#pragma once

#include "driver/twai.h"

void can_init(gpio_num_t tx_pin, gpio_num_t rx_pin);
esp_err_t can_send(uint32_t can_id, uint8_t payload_len, uint8_t *payload);
void can_receive_task(void *pv_parameter);
void can_tx_test_task(void *pv_parameter);
esp_err_t can_loopback_test(void);
