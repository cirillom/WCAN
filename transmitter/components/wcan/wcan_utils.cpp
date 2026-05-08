#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wcan_utils.h"

// ESP-NOW caps at 20 unencrypted peers total.
#define PEER_CACHE_SIZE 20

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    TickType_t last_used;
    bool in_use;
} peer_cache_entry_t;

static peer_cache_entry_t peer_cache[PEER_CACHE_SIZE] = {};

void AddPeer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";

    // Already tracked — refresh LRU timestamp and return.
    for (int i = 0; i < PEER_CACHE_SIZE; i++) {
        if (peer_cache[i].in_use && memcmp(peer_cache[i].mac_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            peer_cache[i].last_used = xTaskGetTickCount();
            return;
        }
    }

    // Find a free slot; if none, pick the LRU occupied slot.
    int slot = 0;
    TickType_t oldest = portMAX_DELAY;
    for (int i = 0; i < PEER_CACHE_SIZE; i++) {
        if (!peer_cache[i].in_use) {
            slot = i;
            oldest = 0;
            break;
        }
        if (peer_cache[i].last_used < oldest) {
            oldest = peer_cache[i].last_used;
            slot = i;
        }
    }

    // Evict the LRU peer to stay within the hardware limit.
    if (peer_cache[slot].in_use) {
        ESP_LOGD(TAG, "Evicting LRU peer %02x:%02x:%02x:%02x:%02x:%02x",
                 peer_cache[slot].mac_addr[0], peer_cache[slot].mac_addr[1],
                 peer_cache[slot].mac_addr[2], peer_cache[slot].mac_addr[3],
                 peer_cache[slot].mac_addr[4], peer_cache[slot].mac_addr[5]);
        esp_now_del_peer(peer_cache[slot].mac_addr);
        peer_cache[slot].in_use = false;
    }

    esp_now_peer_info_t peer = {};
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = ESPNOW_WIFI_IF;
    peer.encrypt = false;
    memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add peer failed: %s", esp_err_to_name(ret));
        return;
    }

    memcpy(peer_cache[slot].mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    peer_cache[slot].last_used = xTaskGetTickCount();
    peer_cache[slot].in_use = true;
    ESP_LOGV(TAG, "Peer added: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
}

void RemovePeer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";
    esp_err_t ret = esp_now_del_peer(mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Remove peer failed: %s", esp_err_to_name(ret));
        return;
    }
    for (int i = 0; i < PEER_CACHE_SIZE; i++) {
        if (peer_cache[i].in_use && memcmp(peer_cache[i].mac_addr, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
            peer_cache[i].in_use = false;
            break;
        }
    }
    ESP_LOGV(TAG, "Peer removed");
}

esp_now_packet_t *EncodeDataPacket(const data_packet_t *data_packet){
    static const char *TAG = "ENCODE";
    esp_now_packet_t *esp_now_packet = (esp_now_packet_t *)malloc(sizeof(esp_now_packet_t));
    ESP_LOGV(TAG, "esp_now_packet: %p\n", (void*)esp_now_packet);
    if (esp_now_packet == NULL) {
        ESP_LOGE(TAG, "Malloc esp now packet fail");
        return NULL;
    } 
    size_t header_len = sizeof(data_packet->can_id) + sizeof(data_packet->tick_count) + sizeof(data_packet->data_count);
    esp_now_packet->data_len = header_len + data_packet->data_count * sizeof(uint32_t);
    esp_now_packet->data = (uint8_t *)malloc(esp_now_packet->data_len);
    ESP_LOGV(TAG, "esp_now_packet->data: %p\n", (void*)esp_now_packet->data);
    if (esp_now_packet->data == NULL) {
        ESP_LOGE(TAG, "Malloc esp now packet fail");
        free(esp_now_packet);
        esp_now_packet = NULL;
        return NULL;
    }
    size_t offset = 0;
    memcpy(esp_now_packet->data + offset, &data_packet->can_id, sizeof(data_packet->can_id));
    offset += sizeof(data_packet->can_id);
    memcpy(esp_now_packet->data + offset, &data_packet->tick_count, sizeof(data_packet->tick_count));
    offset += sizeof(data_packet->tick_count);
    memcpy(esp_now_packet->data + offset, &data_packet->data_count, sizeof(data_packet->data_count));
    offset += sizeof(data_packet->data_count);
    if (data_packet->data_count > 0) {
        memcpy(esp_now_packet->data + offset, data_packet->data, data_packet->data_count * sizeof(uint32_t));
    }
    return esp_now_packet;
}

data_packet_t *DecodeDataPacket(const esp_now_packet_t *esp_now_packet){
    static const char *TAG = "DECODE";
    data_packet_t *data_packet = (data_packet_t *)malloc(sizeof(data_packet_t));
    ESP_LOGV(TAG, "data_packet: %p\n", (void*)data_packet);
    if (data_packet == NULL) {
        ESP_LOGE(TAG, "Malloc data packet fail");
        return NULL;
    }
    memcpy(data_packet->mac_addr, esp_now_packet->mac_addr, ESP_NOW_ETH_ALEN);
    
    size_t offset = 0;
    
    memcpy(&data_packet->can_id, esp_now_packet->data + offset, sizeof(data_packet->can_id));
    offset += sizeof(data_packet->can_id);
    
    memcpy(&data_packet->tick_count, esp_now_packet->data + offset, sizeof(data_packet->tick_count));
    offset += sizeof(data_packet->tick_count);
    
    memcpy(&data_packet->data_count, esp_now_packet->data + offset, sizeof(data_packet->data_count));
    offset += sizeof(data_packet->data_count);

    size_t payload_len = data_packet->data_count * sizeof(uint32_t);
    if (payload_len == 0) {
        data_packet->data = NULL;
    } else {
        data_packet->data = (uint32_t *)malloc(payload_len);
        ESP_LOGV(TAG, "data_packet->data: %p\n", (void*)data_packet->data);
        if (data_packet->data == NULL) {
            ESP_LOGE(TAG, "Malloc payload fail");
            free(data_packet);
            return NULL;
        }
        memcpy(data_packet->data, esp_now_packet->data + offset, payload_len);
    }
    return data_packet;
}

bool DecodeDataPacketInto(const uint8_t *mac_addr, const uint8_t *data, int data_len, data_packet_t *out)
{
    static const char *TAG = "DECODE";
    size_t min_len = sizeof(out->can_id) + sizeof(out->tick_count) + sizeof(out->data_count);
    if (data_len < (int)min_len) {
        ESP_LOGE(TAG, "Packet too short: %d < %u", data_len, (unsigned)min_len);
        return false;
    }

    memcpy(out->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    size_t offset = 0;
    memcpy(&out->can_id, data + offset, sizeof(out->can_id));
    offset += sizeof(out->can_id);
    memcpy(&out->tick_count, data + offset, sizeof(out->tick_count));
    offset += sizeof(out->tick_count);
    memcpy(&out->data_count, data + offset, sizeof(out->data_count));
    offset += sizeof(out->data_count);

    size_t payload_len = out->data_count * sizeof(uint32_t);
    if (payload_len == 0) {
        out->data = NULL;
    } else {
        out->data = (uint32_t *)malloc(payload_len);
        ESP_LOGV(TAG, "data_packet->data: %p\n", (void *)out->data);
        if (out->data == NULL) {
            ESP_LOGE(TAG, "Malloc payload fail");
            return false;
        }
        memcpy(out->data, data + offset, payload_len);
    }
    return true;
}

size_t GetCanTXQueueIndex(uint32_t can_id){
    for (size_t i = 0; i < num_can_queues; i++) {
        if (can_id == tx_can_ids[i]) {
            return i;
        }
    }
    return SIZE_MAX; // Not found
}

uint32_t GetCanIDFromQueueIndex(size_t queue_index){
    if (queue_index < num_can_queues) {
        return tx_can_ids[queue_index];
    }
    return 0; // Invalid index
}
