#ifndef __CAN_H__
#define __CAN_H__

#include "driver/twai.h"
#include "wcan.h"

void CanInit(gpio_num_t tx_pin, gpio_num_t rx_pin);
void RecvCallback(data_packet_t data);

#endif