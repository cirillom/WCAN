#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <utility>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESPNOW_WIFI_MODE_STATION
#define CONFIG_ESPNOW_WIFI_MODE_STATION 1
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Packet.hpp"
#include "RingBuffer.hpp"
#include "Stats.hpp"

namespace wcan {

class TransceiverBase {
public:
    static constexpr size_t TX_RING_SIZE = 10;
    static constexpr size_t RX_RING_SIZE = 20;
    static constexpr uint32_t NOTIFY_BIT_NEW_DATA = (1 << 0);
    static constexpr uint32_t SEND_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t RECV_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t SEND_PROCESSING_TASK_STACK_SIZE = 4096;
    static constexpr uint32_t RECV_PROCESSING_TASK_STACK_SIZE = 6148;
    static constexpr size_t RADIO_TRANSMIT_QUEUE_SIZE = 20;
    static constexpr size_t TX_RESULT_QUEUE_SIZE = 16;
    static constexpr size_t RADIO_MAX_RETRIES = 3;
    static constexpr uint32_t RADIO_TIMEOUT_MS = 150;
    static constexpr uint32_t CONTROL_ID = 0xE0000000;

#if CONFIG_ESPNOW_WIFI_MODE_STATION
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_STA;
#else
    static constexpr esp_mac_type_t MAC_TYPE = ESP_MAC_WIFI_SOFTAP;
#endif

    virtual ~TransceiverBase();

    TransceiverBase(std::vector<CANId_t> rx_can_ids,
                    std::vector<CANId_t> tx_can_ids,
                    uint32_t linger_ms,
                    bool filtering_enabled = false);

    bool init();
    virtual void stop(uint32_t timeout_ms);

    using RecvCallback = std::function<void(const Packet&)>;
    void set_recv_callback(RecvCallback callback) { _recv_callback = std::move(callback); }

    /** @brief Enqueues a data point for a specific CAN ID. Thread-safe. */
    bool send_data(CANId_t can_id, DataPoint_t data);

    /** @brief Seals the current write-head batch and dispatches it. */
    void finish_batch(CANId_t can_id);

    /** @brief Returns the configured TX CAN IDs. */
    const std::vector<CANId_t>& get_tx_can_ids() const { return _tx_can_ids; }

    Stats& stats() { return *_stats; }
    const Stats& stats() const { return *_stats; }

protected:
    bool is_stopping() const { return _stopping; }

    // --- Shared RTOS Resources ---
    QueueHandle_t _radio_transmit_queue = nullptr;
    QueueHandle_t _tx_result_queue = nullptr;
    TaskHandle_t _send_task_handle = nullptr;
    TaskHandle_t _recv_task_handle = nullptr;

    // --- Ring Buffers ---
    std::unordered_map<CANId_t, RingBuffer<Packet, TX_RING_SIZE>> _tx_rings;
    RingBuffer<EspNowPacket, RX_RING_SIZE> _rx_packets;

    // --- Linger Timers (one-shot, per CAN ID) ---
    std::unordered_map<CANId_t, esp_timer_handle_t> _linger_timers;
    // Context structs kept alive for timer callbacks
    std::vector<std::pair<TransceiverBase*, CANId_t>> _linger_timer_contexts;

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

    // --- The Strategy Contract ---
    /**
     * @brief Called by the send task to determine the destination MAC just before calling esp_now_send.
     * @return const uint8_t* The destination MAC, or nullptr for hardware multicast blast.
     */
    virtual const uint8_t* prepare_send_mac(const Packet& packet) = 0;

    /**
     * @brief High-level batch dispatch logic.
     * Broadcast: notifies retry task. Multicast: enqueues to radio_transmit_queue.
     */
    virtual void dispatch_batch(CANId_t can_id) = 0;

    /** @brief Called when a CONTROL_ID packet arrives. */
    virtual void on_control_packet(const Packet& packet) = 0;

    /** @brief Called when a DATA packet arrives (for variant-specific side effects). */
    virtual void on_data_packet(const Packet& packet) {}

    /** @brief Hook for tracking hardware TX results (Overridden by Multicast). */
    virtual void on_hardware_tx_status(const uint8_t* mac_addr, bool success) {}

    /** @brief Called after a radio send completes (success or failure). */
    virtual void on_radio_send(CANId_t can_id, bool success) = 0;

    /** @brief Variant-specific peer setup. */
    virtual bool add_peer(const uint8_t* mac_addr) = 0;

    // --- Helpers ---
    bool start_tasks();
    bool setup_esp_now();

private:
    bool should_accept(CANId_t can_id) const;

    // Task Wrappers
    static void send_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->send_processing_task(); }
    static void recv_task_wrapper(void* param) { static_cast<TransceiverBase*>(param)->recv_processing_task(); }

    void send_processing_task();
    void recv_processing_task();

    static void linger_timer_callback(void* arg);

    static TransceiverBase* s_instance;
    static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);

    RecvCallback _recv_callback = nullptr;
};

} // namespace wcan
