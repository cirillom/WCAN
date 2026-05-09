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
