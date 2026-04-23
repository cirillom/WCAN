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
SemaphoreHandle_t send_semaphore = NULL;
resend_t resend_ctx;

void StartResendScheduler();
void StopResendScheduler();
void ResendData(TimerHandle_t xTimer);

void SendProcessingTask(void *pvParameter)
{
    static const char *TAG = "SEND";
    send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(data_packet_t));
    if (send_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send queue");
        vTaskDelete(NULL);
    }

    send_semaphore = xSemaphoreCreateBinary();
    if (send_semaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create send semaphore");
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Send processing task started");

    data_packet_t send_data;
    while (1)
    {
        if (xQueueReceive(send_queue, &send_data, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGV(TAG, "Processing data with id: %08lx", (unsigned long)send_data.can_id);

            resend_ctx.data_packet = (data_packet_t *)malloc(sizeof(data_packet_t));
            ESP_LOGV(TAG, "resend_ctx.data_packet: %p\n", (void *)resend_ctx.data_packet);
            if (resend_ctx.data_packet == NULL)
            {
                ESP_LOGE(TAG, "Malloc for current send packet fail");
                free(send_data.payload);
                xSemaphoreGive(send_semaphore);
                continue;
            }
            memcpy(resend_ctx.data_packet, &send_data, sizeof(data_packet_t));

            SendData(BROADCAST_MAC, *resend_ctx.data_packet);

            StartResendScheduler();

            xSemaphoreTake(send_semaphore, portMAX_DELAY);

            StopResendScheduler();
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
    ESP_ERROR_CHECK(esp_now_send(esp_now_packet->mac_addr, esp_now_packet->data, esp_now_packet->data_len));
    ESP_LOGI(TAG, "[%08lx] %lu (%lu)", (unsigned long)data_packet.can_id, (unsigned long)*(uint32_t *)(data_packet.payload), (unsigned long)data_packet.tick_count);
    //free packet
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

void StartResendScheduler()
{
    static const char *TAG = "RESEND";

    resend_ctx.retry_count = 0;
    resend_ctx.timer = xTimerCreate("ResendTimer", pdMS_TO_TICKS(WCAN_RETRY_DELAY), pdTRUE, NULL, ResendData);
    if (resend_ctx.timer == NULL)
    {
        ESP_LOGE(TAG, "Create resend timer fail");
        if (resend_ctx.data_packet->payload != NULL)
        {
            free(resend_ctx.data_packet->payload);
            resend_ctx.data_packet->payload = NULL;
            free(resend_ctx.data_packet);
            resend_ctx.data_packet = NULL;
        }
        return;
    }

    xTimerStart(resend_ctx.timer, 0);
    ESP_LOGV(TAG, "Resend timer started");
}

void StopResendScheduler()
{
    static const char *TAG = "RESEND";

    ESP_LOGV(TAG, "Stopping resend timer (%d)", uxSemaphoreGetCount(send_semaphore));

    if (resend_ctx.timer != NULL)
    {
        xTimerStop(resend_ctx.timer, 0);
        xTimerDelete(resend_ctx.timer, 0);
        resend_ctx.timer = NULL;
        ESP_LOGV(TAG, "Resend timer deleted");
    }

    if (resend_ctx.data_packet->payload != NULL)
    {
        free(resend_ctx.data_packet->payload);
        resend_ctx.data_packet->payload = NULL;
    }
    if (resend_ctx.data_packet != NULL)
    {
        free(resend_ctx.data_packet);
        resend_ctx.data_packet = NULL;
    }
}

void ResendData(TimerHandle_t xTimer)
{
    static const char *TAG = "RESEND";

    if (resend_ctx.retry_count < WCAN_MAX_RETRY_COUNT)
    {
        ESP_LOGW(TAG, "Timeout reached, resending %08lx... Attempt: %d of %d",
                 (unsigned long)resend_ctx.data_packet->can_id, resend_ctx.retry_count + 1, WCAN_MAX_RETRY_COUNT);

        SendData(BROADCAST_MAC, *resend_ctx.data_packet);
        resend_ctx.retry_count++;
    }
    else
    {
        ESP_LOGE(TAG, "Max retry attempts reached");
        if (uxSemaphoreGetCount(send_semaphore) == 0)
        {
            xSemaphoreGive(send_semaphore);
            ESP_LOGV(TAG, "Send mutex released");
        }
    }
}

void AckRecv(data_packet_t recv_data)
{
    static const char *TAG = "ACK";

    if (resend_ctx.data_packet == NULL)
    {
        ESP_LOGD(TAG, "ACK received but no pending send");
        return;
    }

    // Extract the acked can_id from the ACK payload
    uint16_t acked_can_id = 0;
    if (recv_data.payload != NULL && recv_data.payload_len >= sizeof(uint16_t))
    {
        memcpy(&acked_can_id, recv_data.payload, sizeof(uint16_t));
    }

    // Check both tick_count AND can_id to avoid false ACK matches
    if (recv_data.tick_count != resend_ctx.data_packet->tick_count || acked_can_id != resend_ctx.data_packet->can_id)
    {
        // must log recv macaddress, can id, payload and tick count and the resend expected tick count
        ESP_LOGW(TAG, "[%08lx] with tick_count %lu, but expected tick_count %lu",
                 (unsigned long)recv_data.payload,
                 (unsigned long)recv_data.tick_count, (unsigned long)resend_ctx.data_packet->tick_count);
        return;
    }
    ESP_LOGD(TAG, "(%lu)", (unsigned long)recv_data.tick_count);

    if (uxSemaphoreGetCount(send_semaphore) == 0)
    {
        xSemaphoreGive(send_semaphore);
        ESP_LOGV(TAG, "Send mutex released");
    }
}
