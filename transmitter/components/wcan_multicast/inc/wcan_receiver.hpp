#pragma once

#include <memory>

#include "wcan.hpp"

#define DEDUP_TABLE_SIZE 32 // one slot per expected CAN ID, size to your network
#define WCAN_DIRECTED_HELLO_MAX_ATTEMPTS 5
#define WCAN_DIRECTED_HELLO_INTERVAL_MS 50
#define WCAN_MAX_PENDING_REGISTRATIONS 8

struct dedup_entry_t {
    uint32_t can_id;
    TickType_t last_tick_count;
    bool valid;
};

void recv_processing_task(void *pv_parameter);
void filter_data(std::unique_ptr<data_packet_t> data);
void registration_init(void);
void registration_on_broadcast(const data_packet_t &data_packet);
void registration_on_multicast(const data_packet_t &data_packet);
