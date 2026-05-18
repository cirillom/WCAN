#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESPNOW_WIFI_MODE_STATION
#define CONFIG_ESPNOW_WIFI_MODE_STATION 1
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Packet.hpp"

namespace wcan {

class TransceiverBase {
public:
    virtual ~TransceiverBase();

    TransceiverBase(std::vector<CANId_t> rx_can_ids,
                    std::vector<CANId_t> tx_can_ids,
                    uint32_t linger_ms,
                    bool filtering_enabled = false);

    bool init();

    using RecvCallback = std::function<void(const Packet&)>;
    void set_recv_callback(RecvCallback callback) { _recv_callback = std::move(callback); }

    /** @brief Enqueues a data point for a specific CAN ID. Thread-safe. */
    bool send_data(CANId_t can_id, DataPoint_t data);

    /** @brief Returns the configured TX CAN IDs. */
    const std::vector<CANId_t>& get_tx_can_ids() const { return _tx_can_ids; }

    /** @brief Helper to find the index of a CAN ID in the TX list. */
    size_t get_can_queue_index(CANId_t can_id) const;

protected:
    // --- Shared RTOS Resources ---
    QueueHandle_t _send_queue = nullptr;
    QueueHandle_t _recv_queue = nullptr;
    std::vector<QueueHandle_t> _can_data_queues;
    QueueHandle_t _tx_result_queue = nullptr;
    std::vector<SemaphoreHandle_t> _ack_semaphores;

    // --- Components ---
    Packet::Deduplicator _deduplicator;

    // --- Configuration ---
    std::array<uint8_t, ESP_NOW_ETH_ALEN> _mac_addr{};
    std::vector<CANId_t> _rx_can_ids;
    std::vector<CANId_t> _tx_can_ids;
    uint32_t _linger_ms;
    bool _filtering_enabled = false;

    // --- The Strategy Contract ---
    /**
     * @brief Called by the send task to determine the destination MAC just before calling esp_now_send.
     * @return const uint8_t* The destination MAC, or nullptr for hardware multicast blast.
     */
    virtual const uint8_t* prepare_send_mac(const Packet& packet) = 0;

    /**
     * @brief High-level batch dispatch logic (e.g. Broadcast retries).
     * Subclass should push to _send_queue and block if necessary.
     */
    virtual void dispatch_packet(const Packet& pkt, size_t queue_index) = 0;

    /** @brief Called when a CONTROL_ID packet arrives. */
    virtual void on_control_packet(const Packet& packet) = 0;

    /** @brief Called when a DATA packet arrives (for variant-specific side effects). */
    virtual void on_data_packet(const Packet& packet) {}

    /** @brief Hook for tracking hardware TX results (Overridden by Multicast). */
    virtual void on_hardware_tx_status(const uint8_t* mac_addr, bool success) {}

    /** @brief Variant-specific peer setup. */
    virtual bool add_peer(const uint8_t* mac_addr) = 0;

    // --- Helpers ---
    void start_tasks();
    bool setup_esp_now();

private:
    bool should_accept(CANId_t can_id) const;

    // Task Wrappers
    static void send_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->send_processing_task(); }
    static void recv_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->recv_processing_task(); }
    static void batch_task_wrapper(void* param) {
        auto* ctx = static_cast<std::pair<TransceiverBase*, size_t>*>(param);
        ctx->first->batch_processing_task(ctx->second);
        delete ctx;
    }

    void send_processing_task();
    void recv_processing_task();
    void batch_processing_task(size_t queue_index);

    static TransceiverBase* s_instance;
    static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);

    RecvCallback _recv_callback = nullptr;

public:
    static constexpr uint32_t SEND_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t RECV_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t BATCH_PROCESSING_TASK_PRIORITY = 5;
    static constexpr size_t QUEUE_SIZE = 100;
    static constexpr size_t RADIO_MAX_RETRIES = 3;
    static constexpr uint32_t RADIO_TIMEOUT_MS = 500;
    static constexpr uint32_t CONTROL_ID = 0xE0000000;

#if CONFIG_ESPNOW_WIFI_MODE_STATION
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_STA;
#else
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_SOFTAP;
#endif
};

} // namespace wcan