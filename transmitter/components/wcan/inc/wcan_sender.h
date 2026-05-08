#ifndef __WCAN_SENDER_H__
#define __WCAN_SENDER_H__

#include "wcan.h"

extern const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN];

#define WCAN_MAX_RETRY_COUNT 10
#define WCAN_RETRY_DELAY_MIN 30 // milliseconds
#define WCAN_RETRY_DELAY_MAX 80 // milliseconds

extern data_packet_t **can_tx_packets;

extern SemaphoreHandle_t espnow_tx_sem;
#define WCAN_TX_SEM_TIMEOUT_MS 500

void CanProcessingTask(void *pvParameter);
void SendProcessingTask(void *pvParameter);
void AckRecv(data_packet_t data);
void SendData(const uint8_t *mac_addr, const data_packet_t data_packet);

#endif