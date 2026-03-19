#ifndef __CAN_H__
#define __CAN_H__

#include "driver/twai.h"
#include "wcan.h"

void CanInit();
void RecvCallback(data_packet_t data);

#endif