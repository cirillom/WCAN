#include <stdio.h>
#include "string.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_heap_trace.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_sender.h"
#include "wcan.h"
#include "wcan_utils.h"

const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t send_queue = NULL;
QueueHandle_t *can_queues = NULL;
SemaphoreHandle_t *can_tx_semaphores = NULL;
data_packet_t **can_tx_packets = NULL;
volatile TickType_t *can_tx_tick_counts = NULL;

SemaphoreHandle_t espnow_tx_sem = NULL;


static data_packet_t *CollectPacket(size_t can_queue_index, const data_packet_t *tmpl)
{
    uint32_t data_point[WCAN_DATA_PACKET_MAX_DATA_COUNT];

    if (xQueueReceive(can_queues[can_queue_index], &data_point[0], portMAX_DELAY) != pdTRUE)
        return NULL;

    int count = 1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(linger_ms);
    while (count < WCAN_DATA_PACKET_MAX_DATA_COUNT)
    {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break; //avoids overflow issues with tick count wraparound
        TickType_t remaining = deadline - now;
        if (xQueueReceive(can_queues[can_queue_index], &data_point[count], remaining) != pdTRUE)
            break;
        count++;
    }

    data_packet_t *packet = (data_packet_t *)malloc(sizeof(data_packet_t));
    if (packet == NULL)
        return NULL;

    *packet = *tmpl;
    packet->data_count = count;
    packet->tick_count = xTaskGetTickCount();
    packet->data = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (packet->data == NULL)
    {
        free(packet);
        return NULL;
    }
    memcpy(packet->data, data_point, count * sizeof(uint32_t));
    return packet;
}

static bool SendWithRetry(size_t can_queue_index, const data_packet_t *packet)
{
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    for (int attempt = 0; attempt < WCAN_MAX_RETRY_COUNT; attempt++)
    {
        uint32_t delay = WCAN_RETRY_DELAY_MIN + (esp_random() % (WCAN_RETRY_DELAY_MAX - WCAN_RETRY_DELAY_MIN + 1));
        if (xSemaphoreTake(can_tx_semaphores[can_queue_index], pdMS_TO_TICKS(delay)) == pdTRUE)
            return true;
        ESP_LOGW(TAG, "Timeout reached, resending %08lx... Attempt: %d of %d",
                 (unsigned long)packet->can_id, attempt + 1, WCAN_MAX_RETRY_COUNT);
        SendData(BROADCAST_MAC, *packet);
    }
    return false;
}

void CanProcessingTask(void *pvParameter)
{
    size_t can_queue_index = (size_t)pvParameter;

    char TAG[20];
    snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", (unsigned int)can_queue_index);

    data_packet_t tmpl = {
        .mac_addr = {0},
        .can_id = GetCanIDFromQueueIndex(can_queue_index),
        .tick_count = 0,
        .data_count = 0,
        .data = NULL,
    };
    memcpy(tmpl.mac_addr, own_mac_addr, ESP_NOW_ETH_ALEN);

    ESP_LOGI(TAG, "CAN processing task %u started", (unsigned int)can_queue_index);

    while (1)
    {
        data_packet_t *packet = CollectPacket(can_queue_index, &tmpl);
        if (packet == NULL)
            continue;

        can_tx_tick_counts[can_queue_index] = packet->tick_count;
        can_tx_packets[can_queue_index] = packet;
        SendData(BROADCAST_MAC, *packet);

        ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu)",
                 (unsigned long)packet->can_id, packet->data_count,
                 (unsigned long)packet->data[0], (unsigned long)packet->data[packet->data_count - 1],
                 (unsigned long)packet->tick_count);

        if (!SendWithRetry(can_queue_index, packet))
            ESP_LOGE(TAG, "Max retry attempts reached for %08lx", (unsigned long)packet->can_id);

        xSemaphoreTake(can_tx_semaphores[can_queue_index], 0);
        can_tx_packets[can_queue_index] = NULL;
        free(packet->data);
        free(packet);
    }
    vTaskDelete(NULL);
}

void SendProcessingTask(void *pvParameter)
{
    static const char *TAG = "SEND";

    ESP_LOGI(TAG, "Send processing task started");

    esp_now_packet_t *esp_now_packet;
    while (1)
    {
        if (xQueueReceive(send_queue, &esp_now_packet, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (xSemaphoreTake(espnow_tx_sem, pdMS_TO_TICKS(WCAN_TX_SEM_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGE(TAG, "TX semaphore timeout — driver may be stuck, dropping packet");
            if (esp_now_packet->data != NULL) { free(esp_now_packet->data); }
            free(esp_now_packet);
            continue;
        }

        esp_err_t err = esp_now_send(esp_now_packet->mac_addr,
                                      esp_now_packet->data,
                                      esp_now_packet->data_len);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_now_send failed synchronously: %s — restoring TX slot",
                     esp_err_to_name(err));
            xSemaphoreGive(espnow_tx_sem);
        }
        if (esp_now_packet->data != NULL)
        {
            free(esp_now_packet->data);
            esp_now_packet->data = NULL;
        }
        free(esp_now_packet);
        esp_now_packet = NULL;
    }
    vTaskDelete(NULL);
}

void SendData(const uint8_t *mac_addr, const data_packet_t data_packet)
{
    static const char *TAG = "SendData";

    esp_now_packet_t *esp_now_packet = EncodeDataPacket(&data_packet);
    if (esp_now_packet == NULL)
    {
        ESP_LOGE(TAG, "EncodeDataPacket failed, dropping packet with CAN ID 0x%08lx", (unsigned long)data_packet.can_id);
        return;
    }
    memcpy(esp_now_packet->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    if (xQueueSend(send_queue, &esp_now_packet, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send queue full, dropping packet with CAN ID 0x%08lx", (unsigned long)data_packet.can_id);
        if (esp_now_packet->data != NULL)
        {
            free(esp_now_packet->data);
            esp_now_packet->data = NULL;
        }
        free(esp_now_packet);
        esp_now_packet = NULL;
    }
}

void AckRecv(data_packet_t recv_data)
{
    static const char *TAG = "ACK";

    if (recv_data.data == NULL || recv_data.data_count < 1)
    {
        ESP_LOGW(TAG, "Malformed ACK: missing payload");
        return;
    }

    uint32_t acked_can_id = 0;
    memcpy(&acked_can_id, recv_data.data, sizeof(uint32_t));

    size_t can_queue_index = GetCanTXQueueIndex(acked_can_id);
    if (can_queue_index == SIZE_MAX)
    {
        ESP_LOGW(TAG, "Received ACK for unknown CAN ID 0x%08lx", (unsigned long)acked_can_id);
        return;
    }

    if (can_tx_packets[can_queue_index] == NULL)
    {
        ESP_LOGW(TAG, "Duplicate ACK ignored for 0x%08lx", (unsigned long)acked_can_id);
        return;
    }

    if (recv_data.tick_count != can_tx_tick_counts[can_queue_index])
    {
        ESP_LOGW(TAG, "[0x%08lx] with tick_count %lu, but expected tick_count %lu",
                 (unsigned long)acked_can_id,
                 (unsigned long)recv_data.tick_count,
                 (unsigned long)can_tx_tick_counts[can_queue_index]);
        return;
    }

    ESP_LOGD(TAG, "Received ACK for packet tick %lu from %02X:%02X:%02X:%02X:%02X:%02X", 
        (unsigned long)recv_data.tick_count, 
        recv_data.mac_addr[0], recv_data.mac_addr[1], recv_data.mac_addr[2], recv_data.mac_addr[3], recv_data.mac_addr[4], recv_data.mac_addr[5]);

    if (uxSemaphoreGetCount(can_tx_semaphores[can_queue_index]) == 0)
    {
        xSemaphoreGive(can_tx_semaphores[can_queue_index]);
        ESP_LOGV(TAG, "Send mutex released");
    }
}