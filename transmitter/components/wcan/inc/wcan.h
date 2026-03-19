#ifndef __wcan_H__
#define __wcan_H__

#include "esp_now.h"
#include "esp_wifi.h"

#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF WIFI_IF_AP
#endif

#define ESPNOW_CHANNEL 1
#define ESPNOW_MAXDELAY 500

typedef struct
{
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint32_t can_id;
  TickType_t tick_count;
  uint8_t data_count;
  uint8_t *payload;
  uint8_t payload_len;
} data_packet_t;

typedef struct
{
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
} esp_now_packet_t;

#define CAN_ACK 0xE0000000
extern bool recv_filter;
extern uint16_t *recv_allowed_ids;
extern size_t recv_allowed_ids_size;

#define RECV_QUEUE_SIZE 10
extern QueueHandle_t recv_queue;
#define SEND_QUEUE_SIZE 10
extern QueueHandle_t send_queue;

void WCAN_Init(bool filter, uint16_t *allowed_ids, size_t allowed_ids_size);
void RecvCallback(data_packet_t data) __attribute__((weak));

#endif
