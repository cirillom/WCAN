#ifndef __WCAN_SENDER_H__
#define __WCAN_SENDER_H__

#include "wcan.h"
#include <stdatomic.h>

extern const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN];

#define WCAN_MAX_RETRY_COUNT 10
#define WCAN_RETRY_DELAY_MIN 10 // milliseconds
#define WCAN_RETRY_DELAY_MAX 30 // milliseconds
typedef struct
{
    data_packet_t *data_packet;
    size_t retry_count;
    TimerHandle_t timer;
    atomic_bool     cancelled;
    atomic_bool     cb_running;
} resend_t;

extern resend_t *can_resend_ctx;

void CanProcessingTask(void *pvParameter);
void SendProcessingTask(void *pvParameter);
void AckRecv(data_packet_t data);
void SendData(const uint8_t *mac_addr, const data_packet_t data_packet);

#endif