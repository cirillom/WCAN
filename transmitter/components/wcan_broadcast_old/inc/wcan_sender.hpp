#pragma once

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wcan.hpp"

extern const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN];

#define WCAN_MAX_RETRY_COUNT 10
#define WCAN_RETRY_DELAY_MIN 30 // milliseconds
#define WCAN_RETRY_DELAY_MAX 80 // milliseconds

extern data_packet_t **can_tx_packets;
extern volatile TickType_t *can_tx_tick_counts;

extern SemaphoreHandle_t espnow_tx_sem;
extern SemaphoreHandle_t espnow_tx_status_sem;
#define WCAN_TX_SEM_TIMEOUT_MS 500
#define WCAN_ACK_SEND_MAX_ATTEMPTS 3
#define WCAN_ACK_RETRY_DELAY_MIN_MS 2
#define WCAN_ACK_RETRY_DELAY_MAX_MS 8

void can_processing_task(void *pv_parameter);
void send_processing_task(void *pv_parameter);
void ack_recv(const data_packet_t &data);
void send_data(const uint8_t *mac_addr, const data_packet_t &data_packet);
bool send_data_and_wait(const uint8_t *mac_addr, const data_packet_t &data_packet);
void sender_on_send_status(esp_now_send_status_t status);

#ifdef MEASURE_INSTR
extern volatile uint64_t g_airtime_total_us;
extern volatile uint64_t g_packets_sent_total;
#endif
