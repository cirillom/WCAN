#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "wcan.hpp"

std::unique_ptr<esp_now_packet_t> encode_data_packet(const data_packet_t &data_packet);

// Decode from a non-owning raw buffer view (used by the ESP-NOW receive callback).
bool decode_data_packet(const uint8_t *mac_addr, const uint8_t *buf, size_t len, data_packet_t &out);

void add_peer(const uint8_t *mac_addr);
void remove_peer(const uint8_t *mac_addr);

size_t get_can_tx_queue_index(uint32_t can_id);
uint32_t get_can_id_from_queue_index(size_t queue_index);
