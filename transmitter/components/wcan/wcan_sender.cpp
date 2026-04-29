#include <stdio.h>
#include "string.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_heap_trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_sender.h"
#include "wcan.h"
#include "wcan_utils.h"

const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t send_queue = NULL;
QueueHandle_t *can_queues = NULL;
SemaphoreHandle_t *can_semaphores = NULL;
resend_t *can_resend_ctx = NULL;

void StartResendScheduler(size_t can_queue_index);
void StopResendScheduler(size_t can_queue_index);
void ResendData(TimerHandle_t xTimer);

void CanProcessingTask(void *pvParameter)
{
    size_t can_queue_index = (size_t)pvParameter;

    char TAG[20];
    snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", (unsigned int)can_queue_index);

    can_queues[can_queue_index] = xQueueCreate(RECV_QUEUE_SIZE, sizeof(uint32_t));
    if (can_queues[can_queue_index] == NULL)
    {
        ESP_LOGE(TAG, "Failed to create CAN queue");
        vTaskDelete(NULL);
    }

    can_semaphores[can_queue_index] = xSemaphoreCreateBinary();
    if (can_semaphores[can_queue_index] == NULL)
    {
        ESP_LOGE(TAG, "Failed to create CAN semaphore");
        vTaskDelete(NULL);
    }

    can_resend_ctx[can_queue_index].timer = NULL;
    can_resend_ctx[can_queue_index].data_packet = NULL;
    can_resend_ctx[can_queue_index].retry_count = 0;

    data_packet_t data_packet = {
        .can_id = GetCanIDFromQueueIndex(can_queue_index),
        .tick_count = 0,
        .data_count = 0,
        .data = NULL};
    memcpy(data_packet.mac_addr, own_mac_addr, ESP_NOW_ETH_ALEN);

    ESP_LOGI(TAG, "CAN processing task %u started", (unsigned int)can_queue_index);

    uint32_t data_point[WCAN_DATA_PACKET_MAX_DATA_COUNT];
    while (1)
    {
        int count = 0;
        if (xQueueReceive(can_queues[can_queue_index], &data_point[0], portMAX_DELAY) == pdTRUE)
        {
            count = 1;
            ESP_LOGV(TAG, "Processing data from: %08lx", GetCanIDFromQueueIndex(can_queue_index));

            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(linger_ms);
            while (count < WCAN_DATA_PACKET_MAX_DATA_COUNT)
            {
                ESP_LOGD(TAG, "Waiting for more data... count=%d", count);
                TickType_t remaining = deadline - xTaskGetTickCount();
                if(remaining <= 0)
                    break;
                if (xQueueReceive(can_queues[can_queue_index], &data_point[count], remaining) == pdTRUE)
                    count++;
                else
                    break;
            }

            ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu]",
                     (unsigned long)GetCanIDFromQueueIndex(can_queue_index), count,
                     (unsigned long)data_point[0], (unsigned long)data_point[count - 1]);
            data_packet_t *packet = (data_packet_t *)malloc(sizeof(data_packet_t));
            if (packet == NULL)
            {
                ESP_LOGE(TAG, "Malloc for current send packet fail");
                continue;
            }

            *packet = data_packet;
            packet->data_count = count;
            packet->data = (uint32_t *)malloc(count * sizeof(uint32_t));
            if (packet->data == NULL)
            {
                ESP_LOGE(TAG, "Malloc for data packet payload fail");
                free(packet);
                continue;
            }
            memcpy(packet->data, data_point, count * sizeof(uint32_t));

            can_resend_ctx[can_queue_index].data_packet = packet;
            ESP_LOGV(TAG, "can_resend_ctx[%u].data_packet: %p\n", (unsigned int)can_queue_index, (void *)packet);

            SendData(BROADCAST_MAC, *can_resend_ctx[can_queue_index].data_packet);

            StartResendScheduler(can_queue_index);

            xSemaphoreTake(can_semaphores[can_queue_index], portMAX_DELAY);

            StopResendScheduler(can_queue_index);
        }
    }
    vTaskDelete(NULL);
}

void SendProcessingTask(void *pvParameter)
{
    static const char *TAG = "SEND";
    send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(esp_now_packet_t *));
    if (send_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send queue");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Send processing task started");

    esp_now_packet_t *esp_now_packet;
    while (1)
    {
        if (xQueueReceive(send_queue, &esp_now_packet, portMAX_DELAY) == pdTRUE)
        {
            ESP_ERROR_CHECK(esp_now_send(esp_now_packet->mac_addr, esp_now_packet->data, esp_now_packet->data_len));

            if (esp_now_packet->data != NULL)
            {
                free(esp_now_packet->data);
                esp_now_packet->data = NULL;
            }
            if (esp_now_packet != NULL)
            {
                free(esp_now_packet);
                esp_now_packet = NULL;
            }
        }
    }
    vTaskDelete(NULL);
}

void SendData(const uint8_t *mac_addr, const data_packet_t data_packet)
{
    static const char *TAG = "SendData";

    esp_now_packet_t *esp_now_packet = EncodeDataPacket(&data_packet);
    memcpy(esp_now_packet->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    PrintCharPacket(esp_now_packet->data, esp_now_packet->data_len);
    if (xQueueSend(send_queue, &esp_now_packet, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Send queue full, dropping packet with CAN ID 0x%08lx", (unsigned long)data_packet.can_id);
        if (esp_now_packet->data != NULL)
        {
            free(esp_now_packet->data);
            esp_now_packet->data = NULL;
        }
        if (esp_now_packet != NULL)
        {
            free(esp_now_packet);
            esp_now_packet = NULL;
        }
    }
}

void StartResendScheduler(size_t can_queue_index)
{
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    can_resend_ctx[can_queue_index].retry_count = 0;
    can_resend_ctx[can_queue_index].timer = xTimerCreate("ResendTimer", pdMS_TO_TICKS(WCAN_RETRY_DELAY), pdTRUE, (void *)can_queue_index, ResendData);
    if (can_resend_ctx[can_queue_index].timer == NULL)
    {
        ESP_LOGE(TAG, "Create resend timer fail");
        if (can_resend_ctx[can_queue_index].data_packet->data != NULL)
        {
            free(can_resend_ctx[can_queue_index].data_packet->data);
            can_resend_ctx[can_queue_index].data_packet->data = NULL;
            free(can_resend_ctx[can_queue_index].data_packet);
            can_resend_ctx[can_queue_index].data_packet = NULL;
        }
        return;
    }

    xTimerStart(can_resend_ctx[can_queue_index].timer, 0);
    ESP_LOGV(TAG, "Resend timer started");
}

void StopResendScheduler(size_t can_queue_index)
{
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    ESP_LOGV(TAG, "Stopping resend timer (%d)", uxSemaphoreGetCount(can_semaphores));

    if (can_resend_ctx[can_queue_index].timer != NULL)
    {
        xTimerStop(can_resend_ctx[can_queue_index].timer, 0);
        xTimerDelete(can_resend_ctx[can_queue_index].timer, 0);
        can_resend_ctx[can_queue_index].timer = NULL;
        ESP_LOGV(TAG, "Resend timer deleted");
    }

    if (can_resend_ctx[can_queue_index].data_packet->data != NULL)
    {
        free(can_resend_ctx[can_queue_index].data_packet->data);
        can_resend_ctx[can_queue_index].data_packet->data = NULL;
    }
    if (can_resend_ctx[can_queue_index].data_packet != NULL)
    {
        free(can_resend_ctx[can_queue_index].data_packet);
        can_resend_ctx[can_queue_index].data_packet = NULL;
    }
}

void ResendData(TimerHandle_t xTimer)
{
    size_t can_queue_index = (size_t)pvTimerGetTimerID(xTimer);
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    // Prevent race condition: If StopResendScheduler already freed the packet
    // (e.g. because an ACK arrived concurrently), safely abort the timeout execution.
    if (can_resend_ctx[can_queue_index].data_packet == NULL)
    {
        return;
    }

    if (can_resend_ctx[can_queue_index].retry_count < WCAN_MAX_RETRY_COUNT)
    {
        ESP_LOGW(TAG, "Timeout reached, resending %08lx... Attempt: %d of %d",
                 (unsigned long)can_resend_ctx[can_queue_index].data_packet->can_id, can_resend_ctx[can_queue_index].retry_count + 1, WCAN_MAX_RETRY_COUNT);

        SendData(BROADCAST_MAC, *can_resend_ctx[can_queue_index].data_packet);
        can_resend_ctx[can_queue_index].retry_count++;
    }
    else
    {
        ESP_LOGE(TAG, "Max retry attempts reached");
        if (uxSemaphoreGetCount(can_semaphores[can_queue_index]) == 0)
        {
            xSemaphoreGive(can_semaphores[can_queue_index]);
            ESP_LOGV(TAG, "Send mutex released");
        }
    }
}

void AckRecv(data_packet_t recv_data)
{
    static const char *TAG = "ACK";

    // Extract the acked can_id from the ACK payload
    uint32_t acked_can_id = 0;
    if (recv_data.data != NULL)
    {
        memcpy(&acked_can_id, recv_data.data, sizeof(uint32_t));
    }
    
    size_t can_queue_index = GetCanTXQueueIndex(acked_can_id);
    if (can_queue_index == SIZE_MAX) {
        ESP_LOGW(TAG, "Received ACK for unknown CAN ID 0x%08lx", (unsigned long)acked_can_id);
        return;
    }

    // If the data packet is NULL, it means the first ACK already cleared it
    // We can safely ignore any subsequent duplicate ACKs from other receivers.
    if (can_resend_ctx[can_queue_index].data_packet == NULL) {
        ESP_LOGD(TAG, "Duplicate ACK ignored for 0x%08lx", (unsigned long)acked_can_id);
        return;
    }

    // Check both tick_count AND can_id to avoid false ACK matches
    if (recv_data.tick_count != can_resend_ctx[can_queue_index].data_packet->tick_count || acked_can_id != can_resend_ctx[can_queue_index].data_packet->can_id)
    {
        // must log recv macaddress, can id, payload and tick count and the resend expected tick count
        ESP_LOGW(TAG, "[%08lx] with tick_count %lu, but expected tick_count %lu",
                 (unsigned long)recv_data.data,
                 (unsigned long)recv_data.tick_count, (unsigned long)can_resend_ctx[can_queue_index].data_packet->tick_count);
        return;
    }
    ESP_LOGD(TAG, "(%lu)", (unsigned long)recv_data.tick_count);

    if (uxSemaphoreGetCount(can_semaphores[can_queue_index]) == 0)
    {
        xSemaphoreGive(can_semaphores[can_queue_index]);
        ESP_LOGV(TAG, "Send mutex released");
    }
}
