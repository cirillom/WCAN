#ifndef __WCAN_RECEIVER_H__
#define __WCAN_RECEIVER_H__

#include "wcan.h"

void RecvProcessingTask(void *pvParameter);
void FilterData(data_packet_t data);

#endif