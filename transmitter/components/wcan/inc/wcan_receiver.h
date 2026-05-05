#ifndef __WCAN_RECEIVER_H__
#define __WCAN_RECEIVER_H__

#include "wcan.h"

#define DEDUP_TABLE_SIZE 32  // one slot per expected CAN ID, size to your network

typedef struct {
    uint32_t can_id;
    TickType_t last_tick_count;
    bool valid;
} dedup_entry_t;

void RecvProcessingTask(void *pvParameter);
void FilterData(data_packet_t data);

#endif