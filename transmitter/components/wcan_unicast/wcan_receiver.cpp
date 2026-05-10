#include <cstdint>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wcan_receiver.hpp"
#include "wcan_sender.hpp"
#include "wcan_utils.hpp"

QueueHandle_t recv_queue = nullptr;

static dedup_entry_t s_dedup_table[DEDUP_TABLE_SIZE] = {};
static SemaphoreHandle_t s_registration_mutex = nullptr;

struct pending_registration_t {
    uint8_t sensor_mac[ESP_NOW_ETH_ALEN];
    TickType_t next_retry_tick;
    uint8_t remaining_attempts;
    bool active;
};

static pending_registration_t s_pending_registrations[WCAN_MAX_PENDING_REGISTRATIONS] = {};

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

static bool is_interested_in_can_id(uint32_t can_id)
{
    if (!recv_filter) {
        return true;
    }

    for (size_t i = 0; i < rx_can_ids_size; i++) {
        if (can_id == rx_can_ids[i]) {
            return true;
        }
    }
    return false;
}

static void send_hello_to(const uint8_t *dest_mac)
{
    data_packet_t hello;
    std::memcpy(hello.mac_addr.data(), own_mac_addr, ESP_NOW_ETH_ALEN);
    hello.can_id = CAN_HELLO;
    hello.tick_count = xTaskGetTickCount();

    if (recv_filter && rx_can_ids != nullptr && rx_can_ids_size > 0) {
        hello.data_count = static_cast<uint8_t>(rx_can_ids_size);
        hello.data = std::make_unique<uint32_t[]>(rx_can_ids_size);
        if (!hello.data) {
            return;
        }
        std::memcpy(hello.data.get(), rx_can_ids, rx_can_ids_size * sizeof(uint32_t));
    } else {
        hello.data_count = 0;
    }

    add_peer(dest_mac);
    send_data(dest_mac, hello);
}

static int find_registration_slot_locked(const uint8_t mac_addr[ESP_NOW_ETH_ALEN])
{
    for (size_t i = 0; i < WCAN_MAX_PENDING_REGISTRATIONS; i++) {
        if (s_pending_registrations[i].active &&
            std::memcmp(s_pending_registrations[i].sensor_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static int allocate_registration_slot_locked(void)
{
    for (size_t i = 0; i < WCAN_MAX_PENDING_REGISTRATIONS; i++) {
        if (!s_pending_registrations[i].active) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

static void registration_retry_task(void *)
{
    static const char *TAG = "REG_RETRY";
    ESP_LOGI(TAG, "Directed registration retry task started");

    while (true) {
        pending_registration_t due_entries[WCAN_MAX_PENDING_REGISTRATIONS] = {};
        size_t due_count = 0;
        const TickType_t now = xTaskGetTickCount();

        if (s_registration_mutex != nullptr &&
            xSemaphoreTake(s_registration_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (size_t i = 0; i < WCAN_MAX_PENDING_REGISTRATIONS; i++) {
                auto &entry = s_pending_registrations[i];
                if (!entry.active || entry.remaining_attempts == 0) {
                    continue;
                }
                if (static_cast<int32_t>(now - entry.next_retry_tick) < 0) {
                    continue;
                }

                due_entries[due_count] = entry;
                due_count++;

                entry.remaining_attempts--;
                if (entry.remaining_attempts == 0) {
                    entry.active = false;
                } else {
                    entry.next_retry_tick = now + pdMS_TO_TICKS(WCAN_DIRECTED_HELLO_INTERVAL_MS);
                }
            }
            xSemaphoreGive(s_registration_mutex);
        }

        for (size_t i = 0; i < due_count; i++) {
            send_hello_to(due_entries[i].sensor_mac);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

        if (wcan_recv_callback) {
            wcan_recv_callback(*pkt);
        } else {
            ESP_LOGW(TAG, "No callback function defined for received data");
        }
        // pkt destructor frees at end of loop iteration.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void filter_data(std::unique_ptr<data_packet_t> data)
{
    static const char *TAG = "FILTER";

    ESP_LOGV(TAG, "Received data with id: %08lx", static_cast<unsigned long>(data->can_id));
    if (!is_interested_in_can_id(data->can_id)) {
        ESP_LOGV(TAG, "Filtered out data with id: %08lx", static_cast<unsigned long>(data->can_id));
        return; // unique_ptr destructor frees
    }

    if (data->received_via_broadcast) {
        registration_on_broadcast(*data);
    } else {
        registration_on_unicast(*data);
    }

    data_packet_t *raw = data.release();
    if (xQueueSend(recv_queue, &raw, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Recv queue full, dropping packet (id: %08lx)", static_cast<unsigned long>(raw->can_id));
        delete raw;
    }
}

void registration_init(void)
{
    if (s_registration_mutex != nullptr) {
        return;
    }

    s_registration_mutex = xSemaphoreCreateMutex();
    if (s_registration_mutex == nullptr) {
        ESP_LOGE("REG_INIT", "Failed to create registration mutex");
        return;
    }

    xTaskCreate(registration_retry_task, "reg_retry", 4096, nullptr, 4, nullptr);
}

void registration_on_broadcast(const data_packet_t &data_packet)
{
    static const char *TAG = "REG_BCAST";

    if (s_registration_mutex == nullptr) {
        return;
    }

    send_hello_to(data_packet.mac_addr.data());

    if (xSemaphoreTake(s_registration_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Registration mutex timeout");
        return;
    }

    int slot = find_registration_slot_locked(data_packet.mac_addr.data());
    if (slot < 0) {
        slot = allocate_registration_slot_locked();
    }

    auto &entry = s_pending_registrations[slot];
    std::memcpy(entry.sensor_mac, data_packet.mac_addr.data(), ESP_NOW_ETH_ALEN);
    entry.remaining_attempts = WCAN_DIRECTED_HELLO_MAX_ATTEMPTS - 1;
    entry.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(WCAN_DIRECTED_HELLO_INTERVAL_MS);
    entry.active = (entry.remaining_attempts > 0);

    xSemaphoreGive(s_registration_mutex);
}

void registration_on_unicast(const data_packet_t &data_packet)
{
    static const char *TAG = "REG_UCAST";

    if (s_registration_mutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(s_registration_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Registration mutex timeout");
        return;
    }

    const int slot = find_registration_slot_locked(data_packet.mac_addr.data());
    if (slot >= 0) {
        s_pending_registrations[slot].active = false;
        s_pending_registrations[slot].remaining_attempts = 0;
    }

    xSemaphoreGive(s_registration_mutex);
}
