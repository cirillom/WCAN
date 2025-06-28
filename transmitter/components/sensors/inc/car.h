#ifndef __CAR_H__
#define __CAR_H__

#include "driver/twai.h"
#include "wcan_communication.h"

void CanInit();
void RecvCallback(data_packet_t data);

#endif