#pragma once

#include <memory>
#include <unordered_map>

#include "TransceiverBase.hpp"

namespace wcan {

class Transceiver : public TransceiverBase {
public:
    using TransceiverBase::TransceiverBase;

    ~Transceiver() override;

    bool init();
    void stop(uint32_t timeout_ms) override;

protected:
    /** @brief Always returns the broadcast MAC address. */
    const uint8_t* prepare_send_mac(const Packet& packet) override;

    /**
     * @brief Notifies the per-CAN-ID retry task that a new batch is ready in the ring.
     */
    void dispatch_batch(CANId_t can_id) override;

    /** @brief Handles incoming ACK packets. */
    void on_control_packet(const Packet& packet) override;

    /** @brief Sends an ACK for received data packets. */
    void on_data_packet(const Packet& packet) override;

    /**
     * @brief Called after radio send completes.
     * On failure, enqueues false to the ACK result queue so the retry task retries immediately.
     */
    void on_radio_send(CANId_t can_id, bool success) override;

    /** @brief Restores receive dedup state when an ACK could not be sent. */
    void on_radio_send_failure_dedup(const Packet& packet);

    /** @brief Broadcast doesn't need specific peer management beyond the initial setup. */
    bool add_peer(const uint8_t* mac_addr) override;

private:
    void retry_processing_task(CANId_t can_id);
    void stop_retry_tasks(uint32_t timeout_ms);
    void delete_ack_queues();

    static void retry_task_wrapper(void* param) {
        auto* ctx = static_cast<std::pair<Transceiver*, CANId_t>*>(param);
        ctx->first->retry_processing_task(ctx->second);
        delete ctx;
    }

    // --- Retry Infrastructure (broadcast-only) ---
    std::unordered_map<CANId_t, TaskHandle_t> _retry_task_handles;
    std::unordered_map<CANId_t, bool> _retry_task_done;
    std::unordered_map<CANId_t, std::shared_ptr<std::atomic<uint32_t>>> _pending_ack_seq_ids;
    std::unordered_map<CANId_t, QueueHandle_t> _ack_result_queues;

    static constexpr size_t PACKET_DELIVERY_ATTEMPTS = 4;
    static constexpr uint32_t PACKET_DELIVERY_TIMEOUT_MIN_MS = 50;
    static constexpr uint32_t PACKET_DELIVERY_TIMEOUT_MAX_MS = 120;
    static constexpr uint32_t RETRY_PROCESSING_TASK_STACK_SIZE = 3072;
    static constexpr uint32_t RETRY_PROCESSING_TASK_PRIORITY = 5;
    static constexpr uint32_t NO_PENDING_ACK_SEQUENCE_ID = UINT32_MAX;
    static constexpr size_t ACK_RESULT_QUEUE_SIZE = 1;
};

} // namespace wcan