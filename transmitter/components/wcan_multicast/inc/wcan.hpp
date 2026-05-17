#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF WIFI_IF_AP
#define ESPNOW_MAC_TYPE ESP_MAC_WIFI_SOFTAP
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#define ESPNOW_MAC_TYPE ESP_MAC_WIFI_STA
#endif

#define ESPNOW_CHANNEL 1
#define ESPNOW_MAXDELAY 500

#define WCAN_DATA_PACKET_HEADER_SIZE (sizeof(uint32_t) + sizeof(TickType_t) + sizeof(uint8_t))
#define WCAN_DATA_PACKET_MAX_DATA_COUNT ((ESP_NOW_MAX_DATA_LEN - WCAN_DATA_PACKET_HEADER_SIZE) / sizeof(uint32_t))

struct data_packet_t {
    std::array<uint8_t, ESP_NOW_ETH_ALEN> mac_addr;
    uint32_t can_id;
    TickType_t tick_count;
    uint8_t data_count;
    bool received_via_broadcast = false;
    std::unique_ptr<uint32_t[]> data;
};

struct esp_now_packet_t {
    std::array<uint8_t, ESP_NOW_ETH_ALEN> mac_addr;
    std::unique_ptr<uint8_t[]> data;
    size_t data_len;
};

// Control-frame sentinels live in the 0xExxxxxxx range (top 3 bits = 0b111),
// which is outside the 29-bit CAN extended ID space (CAN_ID_MAX = 0x1FFFFFFF).
// This component has no application-level ACK; the slot at 0xE0000000 is
// repurposed as the HELLO/discovery sentinel.
#define CAN_HELLO 0xE0000000
#define CAN_ID_MAX 0x1FFFFFFF

extern uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];
extern bool recv_filter;
extern uint32_t *rx_can_ids;
extern size_t rx_can_ids_size;
extern uint32_t linger_ms;
extern uint32_t *tx_can_ids;

#define RECV_QUEUE_SIZE 1000
extern QueueHandle_t recv_queue;
#define SEND_QUEUE_SIZE 1000
extern QueueHandle_t send_queue;
extern size_t num_can_queues;
extern QueueHandle_t *can_queues;

void wcan_init(bool filter, uint32_t *rx_ids, size_t rx_ids_size, uint32_t *tx_ids, size_t tx_ids_size,
               uint32_t linger);

// Weak hook the application defines to consume received data packets.
void wcan_recv_callback(const data_packet_t &recv_packet) __attribute__((weak));
