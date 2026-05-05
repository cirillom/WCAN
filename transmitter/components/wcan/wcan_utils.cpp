#include <string.h>
#include <stdio.h>

#include "esp_log.h"

#include "wcan_utils.h"

void AddPeer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";
    if (esp_now_is_peer_exist(mac_addr)) {
        return;
    }
    
    esp_now_peer_info_t *peer = (esp_now_peer_info_t *)malloc(sizeof(esp_now_peer_info_t));
    ESP_LOGV(TAG, "peer: %p\n", (void*)peer);
    if (peer == NULL) {
        ESP_LOGE(TAG, "Malloc peer information fail");
        return;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    esp_err_t ret = esp_now_add_peer(peer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add peer failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGV(TAG, "Peer added: %02x:%02x:%02x:%02x:%02x:%02x", 
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    }
    free(peer);
}

void RemovePeer(const uint8_t *mac_addr)
{
    static const char *TAG = "PEER";
    esp_err_t ret = esp_now_del_peer(mac_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Remove peer failed: %s", esp_err_to_name(ret));
        return;
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

    size_t payload_len = data_packet->data_count * sizeof(uint32_t); // or esp_now_packet->data_len - header_len
    data_packet->data = (uint32_t *)malloc(payload_len);
    ESP_LOGV(TAG, "data_packet->data: %p\n", (void*)data_packet->data);
    if (data_packet->data == NULL && payload_len > 0) {
        ESP_LOGE(TAG, "Malloc payload fail");
        free(data_packet);
        return NULL;
    }
    if (payload_len > 0) {
        memcpy(data_packet->data, esp_now_packet->data + offset, payload_len);
    }
    return data_packet;
}

void PrintCharPacket(const uint8_t *data, const int data_len){
    // create a string buffer to hold the formatted string
    static const char *TAG = "DATA";
    size_t buf_len = data_len * 3 + 1;
    char *str = (char*)malloc(buf_len);
    ESP_LOGV(TAG, "str: %p\n", (void*)str);
    if (!str) return;
    char *p = str;
    for (size_t i = 0; i < data_len; i++) {
        int written = snprintf(p, 4, "%02x ", data[i]);
        p += written;
    }
    *p = '\0';
    // print the formatted string
    ESP_LOGV(TAG, "%s", str);
    free(str);
}

size_t GetCanTXQueueIndex(uint32_t can_id){
    for (size_t i = 0; i < num_can_queues; i++) {
        if ((can_id & 0xFFFF0000) == (tx_can_ids[i] & 0xFFFF0000)) {
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
