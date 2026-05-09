#include <cstdint>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_receiver.hpp"
#include "wcan_sender.hpp"
#include "wcan_utils.hpp"

QueueHandle_t recv_queue = nullptr;

static void ack_send(const data_packet_t &recv_packet);

static dedup_entry_t s_dedup_table[DEDUP_TABLE_SIZE] = {};

// Stores only the most recent tick per CAN ID. A retransmit carrying an older
// tick that arrives after a newer tick has been recorded will not be suppressed
// - it looks like a new packet. With the current ACK-up-to-N-retries flow this
// is benign because retransmits converge on the same tick, not regress.
static bool is_duplicate(const data_packet_t &pkt)
{
    for (size_t i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (s_dedup_table[i].valid && s_dedup_table[i].can_id == pkt.can_id) {
            if (s_dedup_table[i].last_tick_count == pkt.tick_count) {
                return true;
            }
            s_dedup_table[i].last_tick_count = pkt.tick_count;
            return false;
        }
    }
    // Not seen this CAN ID before - find a free slot
    for (size_t i = 0; i < DEDUP_TABLE_SIZE; i++) {
        if (!s_dedup_table[i].valid) {
            s_dedup_table[i].can_id = pkt.can_id;
            s_dedup_table[i].last_tick_count = pkt.tick_count;
            s_dedup_table[i].valid = true;
            return false;
        }
    }
    // Table full - let it through rather than silently drop
    static bool s_table_full_warned = false;
    if (!s_table_full_warned) {
        ESP_LOGW("is_duplicate", "dedup table full (%d entries); increase DEDUP_TABLE_SIZE", DEDUP_TABLE_SIZE);
        s_table_full_warned = true;
    }
    return false;
}

void recv_processing_task(void *)
{
    static const char *TAG = "RecvProcTask";
    ESP_LOGI(TAG, "Receive processing task started");

    while (true) {
        data_packet_t *raw = nullptr;
        if (xQueueReceive(recv_queue, &raw, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        std::unique_ptr<data_packet_t> pkt(raw);

        ESP_LOGD(TAG, "[%04lx] %lu", static_cast<unsigned long>(pkt->can_id),
                 static_cast<unsigned long>(pkt->tick_count));

        if (is_duplicate(*pkt)) {
            ESP_LOGW(TAG, "Dropping duplicate id=0x%08lx tc=%lu", static_cast<unsigned long>(pkt->can_id),
                     static_cast<unsigned long>(pkt->tick_count));
            continue; // pkt destructor frees
        }

        ack_send(*pkt);
        if (wcan_recv_callback) {
            wcan_recv_callback(*pkt);
        } else {
            ESP_LOGW(TAG, "No callback function defined for received data");
        }
        // pkt destructor frees at end of loop iteration.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void ack_send(const data_packet_t &recv_packet)
{
    static const char *TAG = "ACK";

    ESP_LOGD(TAG, "Acknowledging that received ID: %08lx from %02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned long>(recv_packet.can_id), recv_packet.mac_addr[0], recv_packet.mac_addr[1],
             recv_packet.mac_addr[2], recv_packet.mac_addr[3], recv_packet.mac_addr[4], recv_packet.mac_addr[5]);

    data_packet_t ack_data;
    ack_data.mac_addr = recv_packet.mac_addr;
    ack_data.can_id = CAN_ACK;
    ack_data.tick_count = recv_packet.tick_count;
    ack_data.data = std::make_unique<uint32_t[]>(1);
    if (!ack_data.data) {
        ESP_LOGE(TAG, "Allocate ACK payload failed");
        return;
    }
    ack_data.data[0] = recv_packet.can_id;
    ack_data.data_count = 1;

    add_peer(ack_data.mac_addr.data());
    send_data(ack_data.mac_addr.data(), ack_data);
    ESP_LOGD(TAG, "ACK sent for CAN ID 0x%08lx with tick_count %lu", static_cast<unsigned long>(recv_packet.can_id),
             static_cast<unsigned long>(recv_packet.tick_count));
}

void filter_data(std::unique_ptr<data_packet_t> data)
{
    static const char *TAG = "FILTER";

    ESP_LOGV(TAG, "Received data with id: %08lx", static_cast<unsigned long>(data->can_id));
    if (recv_filter) {
        bool found = false;
        for (size_t i = 0; i < rx_can_ids_size; i++) {
            if (data->can_id == rx_can_ids[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGV(TAG, "Filtered out data with id: %08lx", static_cast<unsigned long>(data->can_id));
            return; // unique_ptr destructor frees
        }
    }

    data_packet_t *raw = data.release();
    if (xQueueSend(recv_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Recv queue full, dropping packet (id: %08lx)", static_cast<unsigned long>(raw->can_id));
        delete raw;
    }
}
