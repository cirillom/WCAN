#include "Packet.hpp"

#include <arpa/inet.h>
#include <cstring>

namespace wcan {

static const char* TAG = "PACKET";

std::optional<Packet> Packet::from_payload(const uint8_t* src_mac,
                                           const uint8_t* payload,
                                           size_t len,
                                           const uint8_t* des_mac) {
    if (len < HEADER_SIZE) {
        ESP_LOGE(TAG, "Payload length %zu is smaller than minimum header size %zu", len, HEADER_SIZE);
        return std::nullopt;
    }

    Packet pkt;

    // 1. Context and Senders
    std::memcpy(pkt._mac_addr.data(), src_mac, ESP_NOW_ETH_ALEN);
    
    if (des_mac != nullptr && std::memcmp(des_mac, BROADCAST_MAC.data(), ESP_NOW_ETH_ALEN) == 0) {
        pkt._received_via_broadcast = true;
    } else {
        pkt._received_via_broadcast = false;
    }

    // 2. Decode Header (Handling Big-Endian Network Byte Order)
    size_t offset = 0;
    
    uint32_t network_can_id = 0;
    std::memcpy(&network_can_id, payload + offset, sizeof(uint32_t));
    pkt._can_id = ntohl(network_can_id);
    offset += sizeof(uint32_t);

    uint32_t network_seq_id = 0;
    std::memcpy(&network_seq_id, payload + offset, sizeof(uint32_t));
    pkt._sequence_id = ntohl(network_seq_id);
    offset += sizeof(uint32_t);

    uint8_t data_count = 0;
    std::memcpy(&data_count, payload + offset, sizeof(uint8_t));
    offset += sizeof(uint8_t);

    // 3. Validate Payload Length
    const size_t expected_payload_len = data_count * DATA_POINT_SIZE;
    if (len < HEADER_SIZE + expected_payload_len) {
        ESP_LOGE(TAG, "Payload truncated. Expected %zu bytes, got %zu", 
                 HEADER_SIZE + expected_payload_len, len);
        return std::nullopt;
    }

    // 4. Decode Data Points (Big-Endian)
    if (data_count > 0) {
        pkt._data.reserve(MAX_DATA_POINTS); // Reserve for efficiency, even if not full
        for (size_t i = 0; i < data_count; i++) {
            uint32_t network_data_point = 0;
            std::memcpy(&network_data_point, payload + offset, DATA_POINT_SIZE);
            if (!pkt.add_data_point(ntohl(network_data_point))) {
                ESP_LOGE(TAG, "Failed to add data point");
                return std::nullopt;
            }
            offset += DATA_POINT_SIZE;
        }
    }

    return pkt;
}

std::optional<std::vector<uint8_t>> Packet::encode() const {
    const size_t data_count = _data.size();
    
    if (data_count > MAX_DATA_POINTS) {
        ESP_LOGE(TAG, "Packet data count %zu exceeds MAX_DATA_POINTS %zu", data_count, MAX_DATA_POINTS);
        return std::nullopt;
    }

    const size_t total_size = HEADER_SIZE + (data_count * DATA_POINT_SIZE);
    std::vector<uint8_t> buffer(total_size);
    
    size_t offset = 0;

    // 1. Encode Header (Big-Endian)
    uint32_t network_can_id = htonl(_can_id);
    std::memcpy(buffer.data() + offset, &network_can_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint32_t network_seq_id = htonl(_sequence_id);
    std::memcpy(buffer.data() + offset, &network_seq_id, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint8_t count_u8 = static_cast<uint8_t>(data_count);
    std::memcpy(buffer.data() + offset, &count_u8, sizeof(uint8_t));
    offset += sizeof(uint8_t);

    // 2. Encode Data Points (Big-Endian)
    for (uint32_t data_point : _data) {
        uint32_t network_data_point = htonl(data_point);
        std::memcpy(buffer.data() + offset, &network_data_point, DATA_POINT_SIZE);
        offset += DATA_POINT_SIZE;
    }

    return buffer;
}

bool Packet::add_data_point(uint32_t data_point) {
    if (_data.size() >= MAX_DATA_POINTS) {
        return false;
    }
    _data.push_back(data_point);
    return true;
}

// -----------------------------------------------------------------------------
// Nested Deduplicator Implementation
// -----------------------------------------------------------------------------

bool Packet::Deduplicator::check_and_update(const Packet& packet) {
    const uint32_t id = packet.get_can_id();
    const uint32_t seq = packet.get_sequence_id();

    // 1. Check existing entries
    for (auto& entry : _history) {
        if (entry.can_id == id) {
            if (entry.sequence_id == seq) {
                return true; // Match found -> Duplicate
            }
            entry.sequence_id = seq; // New sequence for this ID -> Update
            return false;
        }
    }

    // 2. New CAN ID we haven't seen yet
    if (_history.size() < HISTORY_SIZE) {
        _history.push_back({id, seq});
    } else {
        ESP_LOGW(TAG, "Deduplication table full! Cannot track new CAN ID 0x%08lx", static_cast<unsigned long>(id));
        // We let the packet through, but we can't track it for future deduplication
    }
    
    return false;
}

} // namespace wcan