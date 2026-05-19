#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>

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
#include "Stats.hpp"

namespace wcan {

class TransceiverBase {
public:
    virtual ~TransceiverBase();

    TransceiverBase(std::vector<CANId_t> rx_can_ids,
                    std::vector<CANId_t> tx_can_ids,
                    uint32_t linger_ms,
                    bool filtering_enabled = false);

    bool init();
    void stop(uint32_t timeout_ms);

    using RecvCallback = std::function<void(const Packet&)>;
    void set_recv_callback(RecvCallback callback) { _recv_callback = std::move(callback); }

    /** @brief Enqueues a data point for a specific CAN ID. Thread-safe. */
    bool send_data(CANId_t can_id, DataPoint_t data);

    /** @brief Returns the configured TX CAN IDs. */
    const std::vector<CANId_t>& get_tx_can_ids() const { return _tx_can_ids; }

    Stats& stats() { return *_stats; }
    const Stats& stats() const { return *_stats; }

protected:
    bool is_stopping() const { return _stopping; }

    // --- Shared RTOS Resources ---
    // Pool ownership is intentionally simple:
    // free_* queues own unused slots, work queues own in-flight slots, and
    // processing tasks return slots to free_* when done. No packet owns heap data.
    QueueHandle_t _send_queue = nullptr;
    QueueHandle_t _recv_queue = nullptr;
    QueueHandle_t _tx_result_queue = nullptr;
    QueueHandle_t _free_send_packets = nullptr;
    QueueHandle_t _free_rx_packets = nullptr;
    TaskHandle_t _send_task_handle = nullptr;
    TaskHandle_t _recv_task_handle = nullptr;
    std::unordered_map<CANId_t, QueueHandle_t> _can_data_queues;
    std::unordered_map<CANId_t, TaskHandle_t> _batch_task_handles;
    std::unordered_map<CANId_t, bool> _batch_task_done;
    std::unordered_map<CANId_t, uint32_t> _pending_ack_seq_ids;

    // --- Packet Pools ---
    Packet* _send_packet_pool = nullptr;
    EspNowPacket* _rx_packet_pool = nullptr;

    // --- Components ---
    std::unique_ptr<Stats> _stats;
    Packet::Deduplicator _deduplicator;

    // --- Configuration ---
    std::array<uint8_t, ESP_NOW_ETH_ALEN> _mac_addr{};
    std::vector<CANId_t> _rx_can_ids;
    std::vector<CANId_t> _tx_can_ids;
    uint32_t _linger_ms;
    bool _filtering_enabled = false;
    volatile bool _stopping = false;
    volatile bool _send_task_done = false;
    volatile bool _recv_task_done = false;

    Packet* acquire_send_packet(TickType_t wait_ticks);
    bool enqueue_send_packet(Packet* packet, TickType_t wait_ticks);
    void release_send_packet(Packet* packet);

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
    virtual void dispatch_packet(const Packet& pkt, CANId_t can_id) = 0;

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
    bool init_pools();
    EspNowPacket* acquire_rx_packet();
    void release_rx_packet(EspNowPacket* packet);
    
    bool should_accept(CANId_t can_id) const;
    bool queues_drained() const;
    void delete_queues();

    // Task Wrappers
    static void send_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->send_processing_task(); }
    static void recv_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->recv_processing_task(); }
    static void batch_task_wrapper(void* param) {
        auto* ctx = static_cast<std::pair<TransceiverBase*, CANId_t>*>(param);
        ctx->first->batch_processing_task(ctx->second);
        delete ctx;
    }

    void send_processing_task();
    void recv_processing_task();
    void batch_processing_task(CANId_t can_id);

    static TransceiverBase* s_instance;
    static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);

    RecvCallback _recv_callback = nullptr;

public:
    static constexpr uint32_t SEND_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t RECV_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t BATCH_PROCESSING_TASK_PRIORITY = 5;
    static constexpr uint32_t SEND_PROCESSING_TASK_STACK_SIZE = 6144;
    static constexpr uint32_t RECV_PROCESSING_TASK_STACK_SIZE = 6144;
    static constexpr uint32_t BATCH_PROCESSING_TASK_STACK_SIZE = 6144;
    static constexpr size_t SEND_PACKET_POOL_SIZE = 24;
    static constexpr size_t RX_PACKET_POOL_SIZE = 32;
    static constexpr size_t SEND_QUEUE_SIZE = SEND_PACKET_POOL_SIZE;
    static constexpr size_t RECV_QUEUE_SIZE = RX_PACKET_POOL_SIZE;
    static constexpr size_t CAN_DATA_QUEUE_SIZE = 768;
    static constexpr size_t TX_RESULT_QUEUE_SIZE = 16;
    static constexpr size_t RADIO_MAX_RETRIES = 3;
    static constexpr uint32_t RADIO_TIMEOUT_MS = 500;
    static constexpr uint32_t CONTROL_ID = 0xE0000000;
    static constexpr uint32_t NO_PENDING_ACK_SEQUENCE_ID = UINT32_MAX;

#if CONFIG_ESPNOW_WIFI_MODE_STATION
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_STA;
#else
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_SOFTAP;
#endif
};

} // namespace wcan
