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

#define WCAN_DATA_PACKET_HEADER_SIZE (sizeof(uint32_t) + sizeof(TickType_t) + sizeof(uint8_t))
#define WCAN_DATA_PACKET_MAX_DATA_COUNT (ESP_NOW_MAX_DATA_LEN - WCAN_DATA_PACKET_HEADER_SIZE)/ sizeof(uint32_t)

typedef struct
{
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint32_t can_id;
  TickType_t tick_count;
  uint8_t data_count;
  uint32_t *data; //CAN payloads can be up to 8 bytes, we use uint32_t for easier handling
} data_packet_t;

typedef struct
{
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
} esp_now_packet_t;

#define CAN_ACK 0xE0000000
extern uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];
extern bool recv_filter;
extern uint32_t *rx_can_ids;
extern size_t rx_can_ids_size;
extern uint32_t linger_ms;
extern uint32_t *tx_can_ids;

#define RECV_QUEUE_SIZE 200
extern QueueHandle_t recv_queue;
#define SEND_QUEUE_SIZE 200
extern QueueHandle_t send_queue;
extern size_t num_can_queues;
extern QueueHandle_t *can_queues;

extern SemaphoreHandle_t *can_tx_semaphores;

void WCAN_Init(bool _filter, uint32_t *_rx_can_ids, size_t _rx_can_ids_size, uint32_t *_tx_can_ids, size_t _tx_can_ids_size, uint32_t _linger_ms);
void RecvCallback(data_packet_t data) __attribute__((weak));

#endif
