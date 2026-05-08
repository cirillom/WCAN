#include "string.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_receiver.h"
#include "wcan_utils.h"
#include "wcan_sender.h"

QueueHandle_t recv_queue = NULL;

void AckSend(const data_packet_t recv_packet);

static dedup_entry_t dedup_table[DEDUP_TABLE_SIZE] = {};

// Stores only the most recent tick per CAN ID. A retransmit carrying an older
// tick that arrives after a newer tick has been recorded will not be suppressed
// — it looks like a new packet. With the current ACK-up-to-N-retries flow this
// is benign because retransmits converge on the same tick, not regress.
static bool IsDuplicate(const data_packet_t *pkt)
{
    for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (dedup_table[i].valid && dedup_table[i].can_id == pkt->can_id) {
            if (dedup_table[i].last_tick_count == pkt->tick_count) {
                return true;
            }
            dedup_table[i].last_tick_count = pkt->tick_count;
            return false;
        }
    }
    // Not seen this CAN ID before — find a free slot
    for (int i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (!dedup_table[i].valid) {
            dedup_table[i].can_id = pkt->can_id;
            dedup_table[i].last_tick_count = pkt->tick_count;
            dedup_table[i].valid = true;
            return false;
        }
    }
    // Table full — let it through rather than silently drop
    static bool table_full_warned = false;
    if (!table_full_warned) {
        ESP_LOGW("IsDuplicate", "dedup table full (%d entries); increase DEDUP_TABLE_SIZE", DEDUP_TABLE_SIZE);
        table_full_warned = true;
    }
    return false;
}

void RecvProcessingTask(void *pvParameter)
{
    static const char *TAG = "RecvProcTask";
    ESP_LOGI(TAG, "Receive processing task started");

    data_packet_t recv_data_packet;
    while (1)
    {
        if (xQueueReceive(recv_queue, &recv_data_packet, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGD(TAG, "[%04lx] %lu", (unsigned long)recv_data_packet.can_id, (unsigned long)recv_data_packet.tick_count);

            if (IsDuplicate(&recv_data_packet)) {
                ESP_LOGW(TAG, "Dropping duplicate id=0x%08lx tc=%lu",
                        (unsigned long)recv_data_packet.can_id,
                        (unsigned long)recv_data_packet.tick_count);
                continue;
            }

            AckSend(recv_data_packet);
            if (RecvCallback)
            {
                RecvCallback(recv_data_packet);
            }
            else
            {
                ESP_LOGW(TAG, "No callback function defined for received data");
                if (recv_data_packet.data != NULL)
                {
                    free(recv_data_packet.data);
                    recv_data_packet.data = NULL;
                }
                continue;
            }
            if (recv_data_packet.data != NULL)
            {
                free(recv_data_packet.data);
                recv_data_packet.data = NULL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));  // Small delay to prevent task hogging CPU
    }
    vTaskDelete(NULL);
}

void AckSend(const data_packet_t recv_packet)
{
    static const char *TAG = "ACK";

    ESP_LOGD(TAG, "Acknowledging that received ID: %08lx from %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned long)recv_packet.can_id, recv_packet.mac_addr[0], recv_packet.mac_addr[1], recv_packet.mac_addr[2],
             recv_packet.mac_addr[3], recv_packet.mac_addr[4], recv_packet.mac_addr[5]);

    data_packet_t ack_data;
    memcpy(ack_data.mac_addr, recv_packet.mac_addr, ESP_NOW_ETH_ALEN);
    ack_data.can_id = CAN_ACK;
    ack_data.tick_count = recv_packet.tick_count;
    ack_data.data = (uint32_t *)malloc(sizeof(uint32_t));
    ESP_LOGV(TAG, "ack_data.payload: %p\n", (void *)ack_data.data);
    if (ack_data.data == NULL)
    {
        ESP_LOGE(TAG, "Malloc ack payload fail");
        return;
    }
    memcpy(ack_data.data, &recv_packet.can_id, sizeof(uint32_t));
    ack_data.data_count = 1;

    AddPeer(ack_data.mac_addr);
    SendData(ack_data.mac_addr, ack_data);
    free(ack_data.data);
    ESP_LOGD(TAG, "ACK sent for CAN ID 0x%08lx with tick_count %lu", (unsigned long)recv_packet.can_id, (unsigned long)recv_packet.tick_count);
}

void FilterData(data_packet_t data)
{
    static const char *TAG = "FILTER";

    ESP_LOGV(TAG, "Received data with id: %08lx", (unsigned long)data.can_id);
    if (recv_filter)
    {
        bool found = false;
        for (int i = 0; i < rx_can_ids_size; i++)
        {
            if (data.can_id == rx_can_ids[i])
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            ESP_LOGV(TAG, "Filtered out data with id: %08lx", (unsigned long)data.can_id);

            if (data.data != NULL)
            {
                free(data.data);
                data.data = NULL;
            }
            return;
        }
    }

    if (xQueueSend(recv_queue, &data, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Recv queue full, dropping packet (id: %08lx)", (unsigned long)data.can_id);
        if (data.data != NULL)
        {
            free(data.data);
            data.data = NULL;
        }
    }
}
