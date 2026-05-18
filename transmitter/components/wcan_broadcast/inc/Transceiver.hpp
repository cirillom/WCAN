#pragma once

#include "TransceiverBase.hpp"

namespace wcan {

class Transceiver : public TransceiverBase {
public:
    using TransceiverBase::TransceiverBase;

protected:
    /** @brief Always returns the broadcast MAC address. */
    const uint8_t* prepare_send_mac(const Packet& packet) override;

    /** 
     * @brief Pushes to send queue and waits for an application-level ACK. 
     * Retries up to 10 times.
     */
    void dispatch_packet(const Packet& pkt, CANId_t can_id) override;

    /** @brief Handles incoming ACK packets. */
    void on_control_packet(const Packet& packet) override;

    /** @brief Sends an ACK for received data packets. */
    void on_data_packet(const Packet& packet) override;

    /** @brief Broadcast doesn't need specific peer management beyond the initial setup. */
    bool add_peer(const uint8_t* mac_addr) override;

private:
    static constexpr size_t PACKET_DELIVERY_ATTEMPTS = 3;
    static constexpr uint32_t PACKET_DELIVERY_TIMEOUT_MS = 100;
};

} // namespace wcan