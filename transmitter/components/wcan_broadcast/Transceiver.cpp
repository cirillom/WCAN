#include "Transceiver.hpp"

#include <cstring>
#include <algorithm>

namespace wcan {

static const char* TAG = "WCAN_BCAST";

const uint8_t* Transceiver::prepare_send_mac(const Packet& packet) {
    return Packet::BROADCAST_MAC.data();
}

void Transceiver::dispatch_packet(const Packet& pkt, size_t queue_index) {
    for (int i = 0; i < MAX_RETRIES; ++i) {
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
            ESP_LOGE(TAG, "Failed to push packet to send queue");
            return;
        }

        // Wait for ACK semaphore
        if (xSemaphoreTake(_ack_semaphores[queue_index], pdMS_TO_TICKS(ACK_TIMEOUT_MS)) == pdTRUE) {
            // ACK received!
            return;
        }
    }
    
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
    // uint32_t target_seq_id = data[1]; // We could use this for stricter matching if needed

    size_t idx = get_can_queue_index(target_can_id);
    if (idx != SIZE_MAX) {
        xSemaphoreGive(_ack_semaphores[idx]);
    }
}

void Transceiver::on_data_packet(const Packet& packet) {
    // Send ACK back to sender
    Packet ack_pkt(_mac_addr, CONTROL_ID);
    ack_pkt.add_data_point(packet.get_can_id());
    ack_pkt.add_data_point(packet.get_sequence_id());

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
    
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add peer: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace wcan