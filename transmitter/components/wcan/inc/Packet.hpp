#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <optional>
#include "esp_now.h"

namespace wcan {

class Packet {
public:
    /** @brief Atomic counter for thread-safe ID generation. */
    static uint32_t next_sequence_id() {
        static std::atomic<uint32_t> sequence_id{0};
        return sequence_id.fetch_add(1, std::memory_order_relaxed);
    }
    
    Packet() = default;
    
    /** 
     * @brief Constructor for outgoing packets. 
     * Auto-generates a sequence ID. The source_mac will typically be filled by the Transceiver.
     */
    explicit Packet(const std::array<uint8_t, ESP_NOW_ETH_ALEN>& source_mac, uint32_t can_id)
        : _source_mac_addr(source_mac), _can_id(can_id), _sequence_id(next_sequence_id()) {
            _data.reserve(MAX_DATA_POINTS);
        }
        
    /** @brief Factory to create a Packet from incoming raw ESP-NOW data. */
    static std::optional<Packet> from_payload(const uint8_t* src_mac,
                                                const uint8_t* payload,
                                                size_t len,
                                                const uint8_t* des_mac = nullptr);

    /** @brief Encodes for transmission. Returns nullopt if data exceeds ESP-NOW limits. */
    std::optional<std::vector<uint8_t>> encode() const;

    /** @brief Appends a CAN data point. Returns false if packet is full (MAX_DATA_POINTS). */
    bool add_data_point(uint32_t data_point);

    // --- Getters ---
    const std::array<uint8_t, 6>& get_source_mac_addr() const { return _source_mac_addr; }
    uint32_t get_can_id() const { return _can_id; }
    uint32_t get_sequence_id() const { return _sequence_id; }
    const std::vector<uint32_t>& get_data() const { return _data; }
    bool is_received_via_broadcast() const { return _received_via_broadcast; }

private:
    std::array<uint8_t, ESP_NOW_ETH_ALEN> _source_mac_addr{};
    uint32_t _can_id = 0;
    uint32_t _sequence_id = 0;
    std::vector<uint32_t> _data;
    bool _received_via_broadcast = false;

    // Header = ID(4) + Seq(4) + Count(1) = 9 bytes.
    static constexpr size_t DATA_COUNT_SIZE = sizeof(uint8_t);
    static constexpr size_t DATA_POINT_SIZE = sizeof(uint32_t);
    static constexpr size_t HEADER_SIZE = sizeof(_can_id) + sizeof(_sequence_id) + DATA_COUNT_SIZE;
    static constexpr size_t MAX_DATA_POINTS = (ESP_NOW_MAX_DATA_LEN - HEADER_SIZE) / DATA_POINT_SIZE;
    static constexpr std::array<uint8_t, ESP_NOW_ETH_ALEN> BROADCAST_MAC{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

public:
    class Deduplicator{
    public:
        /** @brief Constructs a deduplicator with a history buffer of fixed size. */
        Deduplicator(){_history.reserve(HISTORY_SIZE);}

        /** @brief Checks if a packet is a duplicate and updates the history. */
        bool check_and_update(const Packet& packet);

    private:
        struct Entry {
            uint32_t can_id;
            uint32_t sequence_id;
        };
        std::vector<Entry> _history;

        static constexpr size_t HISTORY_SIZE = 32;
    };
};
}