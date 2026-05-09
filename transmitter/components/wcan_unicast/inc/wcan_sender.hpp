#pragma once

#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wcan.hpp"

extern const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN];

extern SemaphoreHandle_t espnow_tx_sem;
#define WCAN_TX_SEM_TIMEOUT_MS 500

void can_processing_task(void *pv_parameter);
void send_processing_task(void *pv_parameter);
void send_data(const uint8_t *mac_addr, const data_packet_t &data_packet);
