#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "TransceiverBase.hpp"

namespace wcan {

class Transceiver : public TransceiverBase {
public:
    using TransceiverBase::TransceiverBase;

    ~Transceiver() override;

    bool init();

protected:
    const uint8_t* prepare_send_mac(const Packet& packet) override;
    void dispatch_batch(CANId_t can_id) override;
    void on_control_packet(const Packet& packet) override;
    void on_data_packet(const Packet& packet) override;
    void on_hardware_tx_status(const uint8_t* mac_addr, bool success) override;
    void on_radio_send(CANId_t can_id, bool success) override;
    bool add_peer(const uint8_t* mac_addr) override;

private:
    struct MacAddress {
        std::array<uint8_t, ESP_NOW_ETH_ALEN> bytes{};
    };

    struct TxStatusEvent {
        bool has_mac = false;
        bool success = false;
        MacAddress mac{};
    };

    struct PendingControlDestination {
        bool used = false;
        uint32_t sequence_id = 0;
        TickType_t created_at = 0;
        MacAddress mac{};
    };

    class SubscriptionTable {
    public:
        bool update(const uint8_t* mac_addr,
                    std::span<const DataPoint_t> requested_can_ids,
                    const std::vector<CANId_t>& tx_can_ids,
                    TickType_t now);
        bool refresh(const uint8_t* mac_addr, TickType_t now);
        bool remove(const uint8_t* mac_addr);
        size_t collect_alive(CANId_t can_id,
                             TickType_t now,
                             std::array<MacAddress, ESP_NOW_MAX_TOTAL_PEER_NUM - 1>& out) const;

    private:
        struct Entry {
            bool used = false;
            bool accepts_all = false;
            TickType_t last_seen = 0;
            MacAddress mac{};
            std::array<CANId_t, Packet::MAX_DATA_POINTS> can_ids{};
            size_t can_id_count = 0;
        };

        Entry* find(const uint8_t* mac_addr);
        const Entry* find(const uint8_t* mac_addr) const;
        Entry* find_slot_for_update(TickType_t now);
        static bool mac_equals(const MacAddress& lhs, const uint8_t* rhs);
        static bool contains_can_id(const std::vector<CANId_t>& can_ids, CANId_t can_id);
        static bool contains_can_id(std::span<const CANId_t> can_ids, CANId_t can_id);
        static bool is_alive(const Entry& entry, TickType_t now);
        static bool accepts_can_id(const Entry& entry, CANId_t can_id);

        std::array<Entry, ESP_NOW_MAX_TOTAL_PEER_NUM - 1> _entries{};
    };

    bool has_rx_interest() const;
    bool fill_subscription_packet(Packet& packet) const;
    bool send_subscription(const uint8_t* unicast_mac);

    void remember_pending_control_destination(uint32_t sequence_id, const uint8_t* mac_addr);
    bool consume_pending_control_destination(uint32_t sequence_id, MacAddress& out);
    void forget_pending_control_destination(uint32_t sequence_id);

    bool ensure_peer_locked(const uint8_t* mac_addr);
    bool remove_peer_locked(const uint8_t* mac_addr);
    bool sync_peers_for_can_id_locked(CANId_t can_id, size_t* synced_count);
    bool remember_known_peer_locked(const uint8_t* mac_addr);
    void forget_known_peer_locked(const uint8_t* mac_addr);
    bool known_peer_is_target_locked(const MacAddress& peer,
                                     const std::array<MacAddress, ESP_NOW_MAX_TOTAL_PEER_NUM - 1>& targets,
                                     size_t target_count) const;

    void drain_tx_status_events();
    void handle_tx_status_event(const TxStatusEvent& event);
    void management_task();
    void stop_management_task(uint32_t timeout_ms);
    void cleanup_multicast_resources();
    void lock_state();
    void unlock_state();

    static void management_task_wrapper(void* param) { static_cast<Transceiver*>(param)->management_task(); }
    static bool is_broadcast_mac(const uint8_t* mac_addr);
    static bool mac_equals(const MacAddress& lhs, const uint8_t* rhs);
    static bool mac_equals(const MacAddress& lhs, const MacAddress& rhs);
    static void copy_mac(MacAddress& dest, const uint8_t* src);

    static constexpr uint32_t SUBSCRIPTION_INTERVAL_MS = 500;
    static constexpr uint32_t SUBSCRIBER_TTL_MS = 2000;
    static constexpr uint32_t MANAGEMENT_TASK_STACK_SIZE = 4096;
    static constexpr uint32_t MANAGEMENT_TASK_PRIORITY = 4;
    static constexpr size_t TX_STATUS_QUEUE_SIZE = 32;
    static constexpr size_t PENDING_CONTROL_DESTINATIONS = 8;
    static constexpr size_t MAX_UNICAST_PEERS = ESP_NOW_MAX_TOTAL_PEER_NUM - 1;

    SubscriptionTable _subscriptions;
    std::array<PendingControlDestination, PENDING_CONTROL_DESTINATIONS> _pending_control_destinations{};
    std::array<MacAddress, MAX_UNICAST_PEERS> _known_unicast_peers{};
    std::array<bool, MAX_UNICAST_PEERS> _known_unicast_peer_used{};
    MacAddress _prepared_control_destination{};
    QueueHandle_t _tx_status_events = nullptr;
    SemaphoreHandle_t _state_mutex = nullptr;
    TaskHandle_t _management_task_handle = nullptr;
    volatile bool _management_task_done = true;
    bool _broadcast_peer_known = false;
};

} // namespace wcan
