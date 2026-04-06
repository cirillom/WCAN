#ifndef __CAN_H__
#define __CAN_H__

#include "driver/twai.h"

void CanInit(gpio_num_t tx_pin, gpio_num_t rx_pin);
esp_err_t CanSend(uint32_t can_id, uint8_t payload_len, uint8_t *payload);
void CanReceiveTask(void *pvParameter);
void CanTxTestTask(void *pvParameter);
esp_err_t CanLoopbackTest(void);

#endif