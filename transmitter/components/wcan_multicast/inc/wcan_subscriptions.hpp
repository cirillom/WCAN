#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_now.h"
#include "freertos/FreeRTOS.h"

#define WCAN_MAX_SUBSCRIBERS 4
#define WCAN_MAX_IDS_PER_SUBSCRIBER 8
#define WCAN_SUBSCRIBER_LIVENESS_MS 5000
#define WCAN_TX_FAIL_THRESHOLD 3

struct subscriber_entry_t {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint32_t subscribed_ids[WCAN_MAX_IDS_PER_SUBSCRIBER];
    uint8_t num_ids;
    bool wildcard;
    TickType_t last_seen_tick;
    uint8_t consecutive_tx_failures;
    bool in_use;
};

void subscription_init(void);
void subscription_update(const uint8_t mac[ESP_NOW_ETH_ALEN], const uint32_t *ids, size_t n);
void subscription_log_state(void);

// Snapshot the MACs of alive subscribers wanting `can_id` into out_macs.
// Returns the number written (capped at WCAN_MAX_SUBSCRIBERS). Caller iterates
// after the function returns; the subscription mutex is released before the
// caller touches the snapshot, so per-peer side effects (add_peer, send_data)
// don't run under the lock.
size_t alive_subscribers_count(uint32_t can_id,
                                     uint8_t out_macs[WCAN_MAX_SUBSCRIBERS][ESP_NOW_ETH_ALEN]);

// Update per-peer TX health from the espnow_send_cb. Successful multicast
// delivery also refreshes liveness; after K consecutive failures the entry is
// marked dead (in_use=false) and freed for HELLO re-discovery. No-op for MACs
// not in the table (e.g., broadcast).
void subscription_record_tx_status(const uint8_t mac[ESP_NOW_ETH_ALEN], bool success);
