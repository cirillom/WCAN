#include <stdio.h>
#include <stdbool.h>

#include "string.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan.h"
#include "wcan_utils.h"
#include "wcan_sender.h"
#include "wcan_receiver.h"

bool recv_filter = false;
uint32_t *rx_can_ids = NULL;
size_t rx_can_ids_size = 0;
uint32_t linger_ms = 0;
uint32_t *tx_can_ids = NULL;
uint8_t own_mac_addr[ESP_NOW_ETH_ALEN];
size_t num_can_queues = 0;

static void ESPNOW_SendCallback(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    static const char *TAG = "SEND_CB";
    if (mac_addr == NULL)
    {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    if (status == ESP_NOW_SEND_FAIL)
    {
        ESP_LOGW(TAG, "MAC-layer TX failed (no 802.11 ACK from peer)");
    }
    else
    {
        ESP_LOGI(TAG, "Successfully sent packet to %02x%02x",
                 mac_addr[4], mac_addr[5]);
    }

    if (espnow_tx_sem != NULL)
    {
        xSemaphoreGive(espnow_tx_sem);
    }
    else
    {
        ESP_LOGE(TAG, "espnow_tx_sem is NULL in send callback — init order error");
    }
}

static void ESPNOW_RecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    static const char *TAG = "RECV";

    if (recv_info->src_addr == NULL || data == NULL || data_len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    ESP_LOGD(TAG, "Received payload of size %d from %02x:%02x:%02x:%02x:%02x:%02x",
             data_len, recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
             recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);

    esp_now_packet_t recv_packet;
    memcpy(recv_packet.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    recv_packet.data = (uint8_t *)data;
    recv_packet.data_len = data_len;

    data_packet_t recv_data;
    if (!DecodeDataPacket(&recv_packet, &recv_data))
    {
        ESP_LOGE(TAG, "DecodeDataPacket failed");
        return;
    }

    ESP_LOGD(TAG, "Received data with id: %08lx", (unsigned long)recv_data.can_id);
    if (recv_data.can_id == CAN_ACK)
    {
        AckRecv(recv_data);
        if (recv_data.data != NULL)
        {
            free(recv_data.data);
        }
    }
    else
    {
        FilterData(recv_data);
    }
}

static bool CreateHandles(void)
{
    static const char *TAG = "WCAN";

    if (num_can_queues > 0)
    {
        can_queues = (QueueHandle_t *)malloc(num_can_queues * sizeof(QueueHandle_t));
        if (can_queues == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN queues");
            return false;
        }

        can_tx_tasks = (TaskHandle_t *)calloc(num_can_queues, sizeof(TaskHandle_t));
        if (can_tx_tasks == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN task handles");
            free(can_queues);
            can_queues = NULL;
            return false;
        }

        can_tx_packets = (data_packet_t **)malloc(num_can_queues * sizeof(data_packet_t *));
        if (can_tx_packets == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN packet slots");
            free(can_queues);
            can_queues = NULL;
            free(can_tx_tasks);
            can_tx_tasks = NULL;
            return false;
        }

        can_tx_tick_counts = (volatile TickType_t *)calloc(num_can_queues, sizeof(TickType_t));
        if (can_tx_tick_counts == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN tick count slots");
            free(can_queues);
            can_queues = NULL;
            free(can_tx_tasks);
            can_tx_tasks = NULL;
            free(can_tx_packets);
            can_tx_packets = NULL;
            return false;
        }
    }

    espnow_tx_sem = xSemaphoreCreateBinary();
    if (espnow_tx_sem == NULL)
    {
        ESP_LOGE(TAG, "Failed to create espnow_tx_sem");
        return false;
    }
    xSemaphoreGive(espnow_tx_sem);

    send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(esp_now_packet_t *));
    if (send_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send queue");
        return false;
    }

    recv_queue = xQueueCreate(RECV_QUEUE_SIZE, sizeof(data_packet_t));
    if (recv_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create receive queue");
        return false;
    }

    for (size_t i = 0; i < num_can_queues; i++)
    {
        can_queues[i] = xQueueCreate(RECV_QUEUE_SIZE, sizeof(uint32_t));
        if (can_queues[i] == NULL)
        {
            ESP_LOGE(TAG, "Failed to create CAN queue %u", (unsigned int)i);
            return false;
        }

        can_tx_packets[i] = NULL;
    }

    return true;
}

void WCAN_Init(bool _filter, uint32_t *_rx_can_ids, size_t _rx_can_ids_size, uint32_t *_tx_can_ids, size_t _tx_can_ids_size, uint32_t _linger_ms)
{
    static const char *TAG = "WCAN";
    ESP_ERROR_CHECK(esp_now_init());
    ESP_LOGV(TAG, "ESP-NOW initialized");
    AddPeer(BROADCAST_MAC);
    ESP_LOGV(TAG, "Broadcast peer added");

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESPNOW_MAC_TYPE));
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memcpy(own_mac_addr, mac, ESP_NOW_ETH_ALEN);

    recv_filter = _filter;
    if (_filter)
    {
        rx_can_ids = _rx_can_ids;
        rx_can_ids_size = _rx_can_ids_size;
    }
    else
    {
        rx_can_ids = NULL;
        rx_can_ids_size = 0;
    }

    tx_can_ids = _tx_can_ids;
    linger_ms = _linger_ms;
    num_can_queues = _tx_can_ids_size;

    if (!CreateHandles())
    {
        ESP_LOGE(TAG, "Failed to create RTOS handles — aborting init");
        return;
    }

    ESP_ERROR_CHECK(esp_now_register_send_cb(ESPNOW_SendCallback));
    ESP_LOGV(TAG, "ESP-NOW send callback registered");
    ESP_ERROR_CHECK(esp_now_register_recv_cb(ESPNOW_RecvCallback));
    ESP_LOGV(TAG, "ESP-NOW receive callback registered");

    xTaskCreate(SendProcessingTask, "SendProcessingTask", 4096, NULL, 5, NULL);
    // Skip RecvProcessingTask when filter=true with an empty allowlist: ACKs are
    // routed directly through AckRecv and nothing else will ever reach the queue.
    if (!(_filter && _rx_can_ids_size == 0))
    {
        if (!RecvCallback)
        {
            ESP_LOGE(TAG, "RecvCallback is not defined — RecvProcessingTask will not start. "
                          "Define RecvCallback or use filter=true with an empty allowlist.");
            return;
        }
        xTaskCreate(RecvProcessingTask, "RecvProcessingTask", 4096, NULL, 5, NULL);
    }

    for (size_t i = 0; i < num_can_queues; i++)
    {
        char task_name[20];
        snprintf(task_name, sizeof(task_name), "CanProc_%u", (unsigned int)i);
        xTaskCreate(CanProcessingTask, task_name, 4096, (void*)(uintptr_t)i, 4, &can_tx_tasks[i]);
    }

    ESP_LOGI(TAG, "WCAN initialized");
}