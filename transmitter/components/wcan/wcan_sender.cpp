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
SemaphoreHandle_t *can_semaphores = NULL;
resend_t *can_resend_ctx = NULL;

SemaphoreHandle_t espnow_tx_sem = NULL;

void StartResendScheduler(size_t can_queue_index);
void StopResendScheduler(size_t can_queue_index);
void ResendData(TimerHandle_t xTimer);

void CanProcessingTask(void *pvParameter)
{
    size_t can_queue_index = (size_t)pvParameter;

    char TAG[20];
    snprintf(TAG, sizeof(TAG), "CAN_PROC_%u", (unsigned int)can_queue_index);

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
                if (remaining <= 0)
                    break;
                if (xQueueReceive(can_queues[can_queue_index], &data_point[count], remaining) == pdTRUE)
                    count++;
                else
                    break;
            }

            
            data_packet_t *packet = (data_packet_t *)malloc(sizeof(data_packet_t));
            if (packet == NULL)
            {
                ESP_LOGE(TAG, "Malloc for current send packet fail");
                continue;
            }

            *packet = data_packet;
            packet->data_count = count;
            packet->tick_count = xTaskGetTickCount();
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

            ESP_LOGI(TAG, "0x%08lx batch %d [%lu..%lu] at (%lu)",
                     (unsigned long)GetCanIDFromQueueIndex(can_queue_index), count,
                     (unsigned long)data_point[0], (unsigned long)data_point[count - 1],
                     (unsigned long)packet->tick_count);

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
    memcpy(esp_now_packet->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    PrintCharPacket(esp_now_packet->data, esp_now_packet->data_len);
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

void StartResendScheduler(size_t can_queue_index)
{
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    can_resend_ctx[can_queue_index].retry_count = 0;
    atomic_store(&can_resend_ctx[can_queue_index].cancelled, false);
    atomic_store(&can_resend_ctx[can_queue_index].cb_running, false);

    uint32_t initial_delay = WCAN_RETRY_DELAY_MIN + (esp_random() % (WCAN_RETRY_DELAY_MAX - WCAN_RETRY_DELAY_MIN + 1));
    can_resend_ctx[can_queue_index].timer = xTimerCreate("ResendTimer", pdMS_TO_TICKS(initial_delay), pdFALSE, (void *)can_queue_index, ResendData);

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

    ESP_LOGV(TAG, "Stopping resend timer (%d)", uxSemaphoreGetCount(can_semaphores[can_queue_index]));

    if (can_resend_ctx[can_queue_index].timer != NULL)
    {
        xTimerStop(can_resend_ctx[can_queue_index].timer, 0);
        xTimerDelete(can_resend_ctx[can_queue_index].timer, 0);
        while (atomic_load(&can_resend_ctx[can_queue_index].cb_running)) { vTaskDelay(1); }
        can_resend_ctx[can_queue_index].timer = NULL;
        ESP_LOGV(TAG, "Resend timer deleted");
    }

    if (can_resend_ctx[can_queue_index].data_packet != NULL)
    {
        if (can_resend_ctx[can_queue_index].data_packet->data != NULL)
        {
            free(can_resend_ctx[can_queue_index].data_packet->data);
            can_resend_ctx[can_queue_index].data_packet->data = NULL;
        }
        free(can_resend_ctx[can_queue_index].data_packet);
        can_resend_ctx[can_queue_index].data_packet = NULL;
    }
}

void ResendData(TimerHandle_t xTimer)
{
    size_t can_queue_index = (size_t)pvTimerGetTimerID(xTimer);
    char TAG[20];
    snprintf(TAG, sizeof(TAG), "RESEND_%u", (unsigned int)can_queue_index);

    // Must be set before any data_packet dereference. StopResendScheduler
    // spins on cb_running after xTimerStop/Delete; if we set it late, the
    // spinner exits early and frees data_packet while we still hold the pointer.
    atomic_store(&can_resend_ctx[can_queue_index].cb_running, true);

    if (can_resend_ctx[can_queue_index].data_packet == NULL)
    {
        atomic_store(&can_resend_ctx[can_queue_index].cb_running, false);
        return;
    }

    if (can_resend_ctx[can_queue_index].retry_count < WCAN_MAX_RETRY_COUNT)
    {
        ESP_LOGW(TAG, "Timeout reached, resending %08lx... Attempt: %d of %d",
                 (unsigned long)can_resend_ctx[can_queue_index].data_packet->can_id,
                 can_resend_ctx[can_queue_index].retry_count + 1,
                 WCAN_MAX_RETRY_COUNT);

        if (atomic_load(&can_resend_ctx[can_queue_index].cancelled))
        {
            atomic_store(&can_resend_ctx[can_queue_index].cb_running, false);
            return;
        }
        SendData(BROADCAST_MAC, *can_resend_ctx[can_queue_index].data_packet);
        can_resend_ctx[can_queue_index].retry_count++;

        uint32_t next_delay = WCAN_RETRY_DELAY_MIN + (esp_random() % (WCAN_RETRY_DELAY_MAX - WCAN_RETRY_DELAY_MIN + 1));
        xTimerChangePeriod(xTimer, pdMS_TO_TICKS(next_delay), 0);
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

    atomic_store(&can_resend_ctx[can_queue_index].cb_running, false);
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

    if (can_resend_ctx[can_queue_index].data_packet == NULL)
    {
        ESP_LOGW(TAG, "Duplicate ACK ignored for 0x%08lx", (unsigned long)acked_can_id);
        return;
    }

    if (recv_data.tick_count != can_resend_ctx[can_queue_index].data_packet->tick_count ||
        acked_can_id != can_resend_ctx[can_queue_index].data_packet->can_id)
    {
        // Fixed: was logging recv_data.data (the pointer address) instead of acked_can_id
        ESP_LOGW(TAG, "[0x%08lx] with tick_count %lu, but expected tick_count %lu",
                 (unsigned long)acked_can_id,
                 (unsigned long)recv_data.tick_count,
                 (unsigned long)can_resend_ctx[can_queue_index].data_packet->tick_count);
        return;
    }

    ESP_LOGD(TAG, "Received ACK for packet tick %lu from %02X:%02X:%02X:%02X:%02X:%02X", 
        (unsigned long)recv_data.tick_count, 
        recv_data.mac_addr[0], recv_data.mac_addr[1], recv_data.mac_addr[2], recv_data.mac_addr[3], recv_data.mac_addr[4], recv_data.mac_addr[5]);

    if (uxSemaphoreGetCount(can_semaphores[can_queue_index]) == 0)
    {
        atomic_store(&can_resend_ctx[can_queue_index].cancelled, true);
        xSemaphoreGive(can_semaphores[can_queue_index]);
        ESP_LOGV(TAG, "Send mutex released");
    }
}