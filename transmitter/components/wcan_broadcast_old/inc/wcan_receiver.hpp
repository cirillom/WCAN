#pragma once

#include <memory>

#include "wcan.hpp"

#define DEDUP_TABLE_SIZE 32 // one slot per expected CAN ID, size to your network

struct dedup_entry_t {
    uint32_t can_id;
    TickType_t last_tick_count;
    bool valid;
};

void recv_processing_task(void *pv_parameter);
void filter_data(std::unique_ptr<data_packet_t> data);
