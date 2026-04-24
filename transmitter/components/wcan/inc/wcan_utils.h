#ifndef __WCAN_UTILS_H__
#define __WCAN_UTILS_H__

#include "wcan.h"

esp_now_packet_t *EncodeDataPacket(const data_packet_t *data_packet);

data_packet_t *DecodeDataPacket(const esp_now_packet_t *esp_now_packet);

void PrintCharPacket(const uint8_t *data, const int data_len);

void AddPeer(const uint8_t *mac_addr);
void RemovePeer(const uint8_t *mac_addr);

size_t GetCanTXQueueIndex(uint32_t can_id);
uint32_t GetCanIDFromQueueIndex(size_t queue_index);

#endif // __WCAN_UTILS_H__