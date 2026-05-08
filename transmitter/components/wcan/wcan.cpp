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
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(espnow_tx_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    else
    {
        ESP_LOGE(TAG, "espnow_tx_sem is NULL in send callback — init order error");
    }
}

static void ESPNOW_RecvCallback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    static const char *TAG = "RECV";
    esp_now_packet_t *recv_packet = (esp_now_packet_t *)malloc(sizeof(esp_now_packet_t));
    ESP_LOGV(TAG, "recv_packet: %p\n", (void *)recv_packet);
    if (recv_packet == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive cb fail");
        return;
    }

    uint8_t *mac_addr = recv_info->src_addr;
    if (mac_addr == NULL || data == NULL || data_len <= 0)
    {
        ESP_LOGE(TAG, "Receive cb arg error");
        free(recv_packet);
        return;
    }
    memcpy(recv_packet->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    recv_packet->data = (uint8_t *)malloc(data_len);
    ESP_LOGV(TAG, "recv_packet->data: %p\n", (void *)recv_packet->data);
    if (recv_packet->data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        free(recv_packet);
        return;
    }
    memcpy(recv_packet->data, data, data_len);
    recv_packet->data_len = data_len;

    ESP_LOGD(TAG, "Received payload of size %d from %02x:%02x:%02x:%02x:%02x:%02x",
             data_len, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    PrintCharPacket(recv_packet->data, data_len);

    data_packet_t *recv_data = DecodeDataPacket(recv_packet);

    free(recv_packet->data);
    free(recv_packet);
    recv_packet = NULL;

    if (recv_data == NULL)
    {
        ESP_LOGE(TAG, "DecodeDataPacket returned NULL");
        return;
    }

    ESP_LOGD(TAG, "Received data with id: %08lx", (unsigned long)recv_data->can_id);
    if (recv_data->can_id == CAN_ACK)
    {
        AckRecv(*recv_data);
        if (recv_data->data != NULL)
        {
            free(recv_data->data);
            recv_data->data = NULL;
        }
    }
    else
    {
        FilterData(*recv_data);
    }

    if (recv_data != NULL)
    {
        free(recv_data);
        recv_data = NULL;
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

        can_semaphores = (SemaphoreHandle_t *)malloc(num_can_queues * sizeof(SemaphoreHandle_t));
        if (can_semaphores == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN semaphores");
            free(can_queues);
            can_queues = NULL;
            return false;
        }

        can_resend_ctx = (resend_t *)malloc(num_can_queues * sizeof(resend_t));
        if (can_resend_ctx == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for CAN resend context");
            free(can_queues);
            can_queues = NULL;
            free(can_semaphores);
            can_semaphores = NULL;
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

        can_semaphores[i] = xSemaphoreCreateBinary();
        if (can_semaphores[i] == NULL)
        {
            ESP_LOGE(TAG, "Failed to create CAN semaphore %u", (unsigned int)i);
            return false;
        }

        can_resend_ctx[i].timer = NULL;
        can_resend_ctx[i].data_packet = NULL;
        can_resend_ctx[i].retry_count = 0;
        atomic_init(&can_resend_ctx[i].cancelled, false);
        atomic_init(&can_resend_ctx[i].cb_running, false);
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
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
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
        const char *task_name = (const char *)malloc(20);
        snprintf((char *)task_name, 20, "CanProc_%u", (unsigned int)i);
        xTaskCreate(CanProcessingTask, task_name, 4096, (void*)(uintptr_t)i, 4, NULL);
        free((void *)task_name);
    }

    ESP_ERROR_CHECK(esp_now_register_send_cb(ESPNOW_SendCallback));
    ESP_LOGV(TAG, "ESP-NOW send callback registered");
    ESP_ERROR_CHECK(esp_now_register_recv_cb(ESPNOW_RecvCallback));
    ESP_LOGV(TAG, "ESP-NOW receive callback registered");

    ESP_LOGI(TAG, "WCAN initialized");
}