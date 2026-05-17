#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"

#include "Packet.hpp"

namespace wcan{
class TransceiverBase{
public:
    virtual ~TransceiverBase();

    /** @brief Constructor for the TransceiverBase class. */
    TransceiverBase(std::vector<uint32_t> rx_can_ids, std::vector<uint32_t> tx_can_ids, uint32_t linger_ms)
        : _rx_can_ids(std::move(rx_can_ids)), 
        _tx_can_ids(std::move(tx_can_ids)), 
        _linger_ms(linger_ms),
        _filtering_enabled(!_rx_can_ids.empty()) {}

    /** @brief Initializes the TransceiverBase class. */
    bool init();

    /** @brief Sets the callback for received packets. */
    using RecvCallback = std::function<void(const Packet&)>;
    void set_recv_callback(RecvCallback callback) { _recv_callback = std::move(callback); }

protected:
    std::array<uint8_t, ESP_NOW_ETH_ALEN> mac_addr{};
    std::vector<uint32_t> _rx_can_ids;
    std::vector<uint32_t> _tx_can_ids;
    uint32_t _linger_ms;
    bool _filtering_enabled = false;

    QueueHandle_t _send_queue = nullptr;
    QueueHandle_t _recv_queue = nullptr;
    std::vector<QueueHandle_t> _can_queues;

    //radio mutex
    SemaphoreHandle_t _radio_semaphore = nullptr;
    std::vector<SemaphoreHandle_t> _ack_semaphores;

    Packet::Deduplicator _deduplicator;

    virtual void on_control_packet(const Packet& packet) = 0;
    virtual void send_strategy(const Packet& packet) = 0;

    void start_tasks();
    void setup_esp_now();
    size_t get_can_queue_index(uint32_t can_id) const;
private:
    static void send_task_wrapper(void* param){ static_cast<TransceiverBase*>(param)->send_processing_task(); }
    static void recv_task_wrapper(void* param){ static_cast<TransceiverBase*>(param)->recv_processing_task(); }
    static void can_task_wrapper(void* param){ 
        auto ctx = static_cast<std::pair<TransceiverBase*, size_t>*>(param);
        ctx->first->can_processing_task(ctx->second);
        delete ctx;
    }

    void send_processing_task();
    void recv_processing_task();
    void can_processing_task(size_t queue_index);
    
    static TransceiverBase* s_instance = nullptr;
    static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);
    static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len);

    RecvCallback _recv_callback = nullptr;

    static constexpr uint32_t SEND_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t RECV_PROCESSING_TASK_PRIORITY = 6;
    static constexpr uint32_t CAN_PROCESSING_TASK_PRIORITY = 5;
    static constexpr uint32_t CONTROL_ID = 0xE0000000;
};
}