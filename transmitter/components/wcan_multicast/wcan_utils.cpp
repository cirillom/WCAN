#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_utils.hpp"

// ESP-NOW caps at 20 unencrypted peers total.
#define PEER_CACHE_SIZE 20

struct peer_cache_entry_t {
    std::array<uint8_t, ESP_NOW_ETH_ALEN> mac_addr;
    TickType_t last_used;
    bool in_use;
};

static peer_cache_entry_t s_peer_cache[PEER_CACHE_SIZE] = {};

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return std::memcmp(a, b, ESP_NOW_ETH_ALEN) == 0;
}

static bool target_contains(const uint8_t targets[][ESP_NOW_ETH_ALEN], size_t target_count,
                            const uint8_t *mac_addr)
{
    for (size_t i = 0; i < target_count; i++) {
        if (mac_equal(targets[i], mac_addr)) {
            return true;
        }
    }
    return false;
}

bool add_peer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";

    // Already tracked - refresh LRU timestamp and return.
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (s_peer_cache[i].in_use && mac_equal(s_peer_cache[i].mac_addr.data(), mac_addr)) {
            s_peer_cache[i].last_used = xTaskGetTickCount();
            return true;
        }
    }

    // Find a free slot; if none, pick the LRU occupied slot.
    size_t slot = 0;
    TickType_t oldest = portMAX_DELAY;
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (!s_peer_cache[i].in_use) {
            slot = i;
            oldest = 0;
            break;
        }
        if (s_peer_cache[i].last_used < oldest) {
            oldest = s_peer_cache[i].last_used;
            slot = i;
        }
    }

    // Evict the LRU peer to stay within the hardware limit.
    if (s_peer_cache[slot].in_use) {
        ESP_LOGD(TAG, "Evicting LRU peer %02x:%02x:%02x:%02x:%02x:%02x", s_peer_cache[slot].mac_addr[0],
                 s_peer_cache[slot].mac_addr[1], s_peer_cache[slot].mac_addr[2], s_peer_cache[slot].mac_addr[3],
                 s_peer_cache[slot].mac_addr[4], s_peer_cache[slot].mac_addr[5]);
        esp_now_del_peer(s_peer_cache[slot].mac_addr.data());
        s_peer_cache[slot].in_use = false;
    }

    esp_now_peer_info_t peer = {};
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = ESPNOW_WIFI_IF;
    peer.encrypt = false;
    std::memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Add peer failed: %s", esp_err_to_name(ret));
        return false;
    }

    std::memcpy(s_peer_cache[slot].mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);
    s_peer_cache[slot].last_used = xTaskGetTickCount();
    s_peer_cache[slot].in_use = true;
    ESP_LOGV(TAG, "Peer added: %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
             mac_addr[4], mac_addr[5]);
    return true;
}

void remove_peer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";
    esp_err_t ret = esp_now_del_peer(mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Remove peer failed: %s", esp_err_to_name(ret));
        return;
    }
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (s_peer_cache[i].in_use && std::memcmp(s_peer_cache[i].mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            s_peer_cache[i].in_use = false;
            break;
        }
    }
    ESP_LOGV(TAG, "Peer removed");
}

void sync_multicast_peers(const uint8_t targets[][ESP_NOW_ETH_ALEN], size_t target_count)
{
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (!s_peer_cache[i].in_use) {
            continue;
        }
        if (target_contains(targets, target_count, s_peer_cache[i].mac_addr.data())) {
            continue;
        }
        remove_peer(s_peer_cache[i].mac_addr.data());
    }

    for (size_t i = 0; i < target_count; i++) {
        add_peer(targets[i]);
    }
}

std::unique_ptr<esp_now_packet_t> encode_data_packet(const data_packet_t &data_packet)
{
    static const char *TAG = "ENCODE";

    auto pkt = std::make_unique<esp_now_packet_t>();
    const size_t header_len =
        sizeof(data_packet.can_id) + sizeof(data_packet.tick_count) + sizeof(data_packet.data_count);
    pkt->data_len = header_len + data_packet.data_count * sizeof(uint32_t);
    pkt->data = std::make_unique<uint8_t[]>(pkt->data_len);
    if (!pkt->data) {
        ESP_LOGE(TAG, "Allocate ESP-NOW payload failed");
        return nullptr;
    }

    size_t offset = 0;
    std::memcpy(pkt->data.get() + offset, &data_packet.can_id, sizeof(data_packet.can_id));
    offset += sizeof(data_packet.can_id);
    std::memcpy(pkt->data.get() + offset, &data_packet.tick_count, sizeof(data_packet.tick_count));
    offset += sizeof(data_packet.tick_count);
    std::memcpy(pkt->data.get() + offset, &data_packet.data_count, sizeof(data_packet.data_count));
    offset += sizeof(data_packet.data_count);
    if (data_packet.data_count > 0 && data_packet.data) {
        std::memcpy(pkt->data.get() + offset, data_packet.data.get(), data_packet.data_count * sizeof(uint32_t));
    }
    return pkt;
}

bool decode_data_packet(const uint8_t *mac_addr, const uint8_t *buf, size_t len, data_packet_t &out)
{
    static const char *TAG = "DECODE";
    const size_t min_len = sizeof(out.can_id) + sizeof(out.tick_count) + sizeof(out.data_count);
    if (len < min_len) {
        ESP_LOGE(TAG, "Packet too short: %zu < %zu", len, min_len);
        return false;
    }

    std::memcpy(out.mac_addr.data(), mac_addr, ESP_NOW_ETH_ALEN);

    size_t offset = 0;
    std::memcpy(&out.can_id, buf + offset, sizeof(out.can_id));
    offset += sizeof(out.can_id);
    std::memcpy(&out.tick_count, buf + offset, sizeof(out.tick_count));
    offset += sizeof(out.tick_count);
    std::memcpy(&out.data_count, buf + offset, sizeof(out.data_count));
    offset += sizeof(out.data_count);

    const size_t payload_len = static_cast<size_t>(out.data_count) * sizeof(uint32_t);
    if (payload_len == 0) {
        out.data.reset();
    } else {
        out.data = std::make_unique<uint32_t[]>(out.data_count);
        if (!out.data) {
            ESP_LOGE(TAG, "Allocate decoded payload failed");
            return false;
        }
        std::memcpy(out.data.get(), buf + offset, payload_len);
    }
    return true;
}

size_t get_can_tx_queue_index(uint32_t can_id)
{
    for (size_t i = 0; i < num_can_queues; i++) {
        if (can_id == tx_can_ids[i]) {
            return i;
        }
    }
    return SIZE_MAX;
}

uint32_t get_can_id_from_queue_index(size_t queue_index)
{
    if (queue_index < num_can_queues) {
        return tx_can_ids[queue_index];
    }
    return 0;
}
