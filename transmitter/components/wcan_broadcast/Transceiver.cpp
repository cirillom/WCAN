#include "Transceiver.hpp"

#include <cstring>
#include <algorithm>

namespace wcan {

static const char* TAG = "WCAN_BCAST";

const uint8_t* Transceiver::prepare_send_mac(const Packet& packet) {
    if (packet.get_can_id() == CONTROL_ID) {
        static uint8_t dest_mac[ESP_NOW_ETH_ALEN];
        const auto& data = packet.get_data();
        
        if (data.size() >= 4) {
            std::memcpy(dest_mac, &data[2], 4);
            std::memcpy(dest_mac + 4, &data[3], 2);
            return dest_mac;
        }
    }
    return Packet::BROADCAST_MAC.data();
}

void Transceiver::dispatch_packet(const Packet& pkt, CANId_t can_id) {
    _pending_ack_seq_ids[can_id] = pkt.get_sequence_id();
    (void)ulTaskNotifyTake(pdTRUE, 0);

    for (int i = 0; i < PACKET_DELIVERY_ATTEMPTS; ++i) {
        // Create a new packet on the heap to be owned by the send_task
        Packet* to_send = new Packet(pkt);

        std::printf("P(%i):%lu:%lx:%lu:%lu:%lu:%u\n",
                    i + 1,
                    (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                    static_cast<unsigned long>(to_send->get_can_id()),
                    static_cast<unsigned long>(to_send->get_sequence_id()),
                    static_cast<unsigned long>(to_send->get_data().front()),
                    static_cast<unsigned long>(to_send->get_data().back()),
                    (unsigned int)to_send->get_data().size());
        
        if (xQueueSend(_send_queue, &to_send, portMAX_DELAY) != pdTRUE) {
            delete to_send;
            _pending_ack_seq_ids[can_id] = NO_PENDING_ACK_SEQUENCE_ID;
            ESP_LOGE(TAG, "Failed to push packet to send queue");
            return;
        }

        // Wait for ACK semaphore
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PACKET_DELIVERY_TIMEOUT_MS)) > 0) {
            _pending_ack_seq_ids[can_id] = NO_PENDING_ACK_SEQUENCE_ID;
            // ACK received!
            return;
        }
    }

    _pending_ack_seq_ids[can_id] = NO_PENDING_ACK_SEQUENCE_ID;
    
    std::printf("P(FAIL):%lu:%lx:%lu:%lu:%lu:%u\n",
                (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                static_cast<unsigned long>(pkt.get_can_id()),
                static_cast<unsigned long>(pkt.get_sequence_id()),
                static_cast<unsigned long>(pkt.get_data().front()),
                static_cast<unsigned long>(pkt.get_data().back()),
                (unsigned int)pkt.get_data().size());
}

void Transceiver::on_control_packet(const Packet& packet) {
    const auto& data = packet.get_data();
    if (data.size() < 2) return;

    uint32_t target_can_id = data[0];
    uint32_t target_seq_id = data[1]; // We could use this for stricter matching if needed

    auto it = _batch_task_handles.find(target_can_id);
    if (it != _batch_task_handles.end() && it->second != nullptr &&
        _pending_ack_seq_ids[target_can_id] == target_seq_id) {
        xTaskNotifyGive(it->second);
    }
}

void Transceiver::on_data_packet(const Packet& packet) {
    // Send ACK back to sender
    Packet ack_pkt(_mac_addr, CONTROL_ID);
    ack_pkt.add_data_point(packet.get_can_id());
    ack_pkt.add_data_point(packet.get_sequence_id());

    const auto& dest_mac = packet.get_source_mac_addr();
    if (!add_peer(dest_mac.data())) {
        ESP_LOGE(TAG, "Failed to add ACK peer");
        return;
    }

    uint32_t mac_part1, mac_part2;
    std::memcpy(&mac_part1, dest_mac.data(), 4);
    std::memcpy(&mac_part2, dest_mac.data() + 4, 2);
    ack_pkt.add_data_point(mac_part1);
    ack_pkt.add_data_point(mac_part2);

    Packet* to_send = new Packet(ack_pkt);
    if (xQueueSend(_send_queue, &to_send, portMAX_DELAY) != pdTRUE) {
        delete to_send;
        ESP_LOGE(TAG, "Failed to send ACK");
    }
}

bool Transceiver::add_peer(const uint8_t* mac_addr) {
    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    peer.channel = 0; // Use current channel
    peer.encrypt = false;
    
    if (esp_now_is_peer_exist(peer.peer_addr)) return true;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace wcan