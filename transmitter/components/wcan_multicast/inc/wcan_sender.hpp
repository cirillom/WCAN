#pragma once

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wcan.hpp"

enum class delivery_mode_t : uint8_t {
    MULTICAST_ACTIVE = 0,
    BROADCAST_DISCOVERY,
};

extern const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN];

extern SemaphoreHandle_t espnow_tx_sem;
extern SemaphoreHandle_t espnow_tx_status_sem;
#define WCAN_TX_SEM_TIMEOUT_MS 500
#define WCAN_MULTICAST_BATCH_MAX_ATTEMPTS 3
#define WCAN_MULTICAST_POST_SUCCESS_DRAIN_MS 2
#define WCAN_MULTICAST_FAIL_TIMEOUT_MS 20

void can_processing_task(void *pv_parameter);
void send_processing_task(void *pv_parameter);
void send_data(const uint8_t *mac_addr, const data_packet_t &data_packet);
bool send_data_and_wait(const uint8_t *mac_addr, const data_packet_t &data_packet);
void sender_init_delivery_state(void);
void sender_on_subscription_update(const uint32_t *ids, size_t n);
void sender_on_send_status(const uint8_t *mac_addr, esp_now_send_status_t status);

#ifdef MEASURE_INSTR
// In-flight TX state populated by send_processing_task before esp_now_send,
// read by espnow_send_cb to compute HW-ACK turnaround latency.
extern volatile int64_t g_in_flight_send_us;
extern volatile uint8_t g_in_flight_peer_mac[ESP_NOW_ETH_ALEN];
extern volatile uint64_t g_airtime_total_us;
extern volatile uint64_t g_packets_sent_total;

void measure_start(void);
#endif
