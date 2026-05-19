#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include "esp_now.h"

namespace wcan {

typedef uint32_t CANId_t;
typedef uint32_t DataPoint_t;
typedef uint16_t DataCount_t;

#ifdef ESP_NOW_MAX_DATA_LEN_V2
#define ESP_NOW_MAX_DATA_LENGTH ESP_NOW_MAX_DATA_LEN_V2
#else
#define ESP_NOW_MAX_DATA_LENGTH ESP_NOW_MAX_DATA_LEN
#endif

struct EspNowPacket{
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
    uint8_t des_mac[ESP_NOW_ETH_ALEN];
    uint8_t payload[ESP_NOW_MAX_DATA_LENGTH];
    size_t payload_len;

    static uint32_t extract_can_id(const uint8_t* payload) {
        CANId_t can_id;
        std::memcpy(&can_id, payload, sizeof(CANId_t));
        return can_id;
    }
};

class Packet {
public:
    static constexpr size_t HEADER_SIZE = sizeof(CANId_t) + sizeof(uint32_t) + sizeof(DataCount_t);
    static constexpr size_t MAX_DATA_POINTS = (ESP_NOW_MAX_DATA_LENGTH - HEADER_SIZE) / sizeof(DataPoint_t);
    static constexpr std::array<uint8_t, ESP_NOW_ETH_ALEN> BROADCAST_MAC{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
    explicit Packet(const std::array<uint8_t, ESP_NOW_ETH_ALEN>& source_mac, CANId_t can_id)
        : _source_mac_addr(source_mac), _can_id(can_id), _sequence_id(next_sequence_id()) {}

    void clear();

    /** @brief Decodes raw ESP-NOW payload into an existing Packet without heap allocation. */
    static bool from_payload(const uint8_t* src_mac,
                             const uint8_t* payload,
                             size_t len,
                             Packet& out,
                             const uint8_t* des_mac = nullptr);

    /** @brief Encodes for transmission into caller-owned storage. */
    bool encode(uint8_t* buffer, size_t capacity, size_t* written) const;

    /** @brief Appends a CAN data point. Returns false if packet is full (MAX_DATA_POINTS). */
    bool add_data_point(DataPoint_t data_point);

    // --- Getters ---
    const std::array<uint8_t, ESP_NOW_ETH_ALEN>& get_source_mac_addr() const { return _source_mac_addr; }
    CANId_t get_can_id() const { return _can_id; }
    uint32_t get_sequence_id() const { return _sequence_id; }
    std::span<const DataPoint_t> get_data() const { return {_data.data(), _data_count}; }
    bool is_received_via_broadcast() const { return _received_via_broadcast; }

private:
    std::array<uint8_t, ESP_NOW_ETH_ALEN> _source_mac_addr{};
    CANId_t _can_id = 0;
    uint32_t _sequence_id = 0;
    std::array<DataPoint_t, MAX_DATA_POINTS> _data{};
    DataCount_t _data_count = 0;
    bool _received_via_broadcast = false;

public:
    class Deduplicator{
    public:
        /** @brief Constructs a deduplicator with a history buffer of fixed size. */
        Deduplicator(){_history.reserve(HISTORY_SIZE);}

        /** @brief Checks if a packet is a duplicate and updates the history. */
        bool check_and_update(const Packet& packet);

    private:
        struct Entry {
            CANId_t can_id;
            uint32_t sequence_id;
        };
        std::vector<Entry> _history;

        static constexpr size_t HISTORY_SIZE = 32;
    };
};
} // namespace wcan
