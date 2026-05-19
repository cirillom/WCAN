#include "Transceiver.hpp"

#include <cstring>
#include <algorithm>

namespace wcan {

static const char* TAG = "WCAN_BCAST";

const uint8_t* Transceiver::prepare_send_mac(const Packet& packet) {
    if (packet.get_can_id() == CONTROL_ID) {
        static uint8_t dest_mac[ESP_NOW_ETH_ALEN];
        const auto data = packet.get_data();

        if (data.size() >= 4) {
            std::memcpy(dest_mac, &data[2], 4);
            std::memcpy(dest_mac + 4, &data[3], 2);
            return dest_mac;
        }
    }
    return Packet::BROADCAST_MAC.data();
}

void Transceiver::dispatch_packet(const Packet& pkt, CANId_t can_id) {
    _pending_ack_seq_ids[can_id]->store(pkt.get_sequence_id());
    (void)ulTaskNotifyTake(pdTRUE, 0);

    for (int i = 0; i < PACKET_DELIVERY_ATTEMPTS; ++i) {
        const TickType_t send_wait = is_stopping() ? 0 : portMAX_DELAY;
        Packet* to_send = acquire_packet(send_wait);
        if (to_send == nullptr) {
            _pending_ack_seq_ids[can_id]->store(NO_PENDING_ACK_SEQUENCE_ID);
            ESP_LOGE(TAG, "Send packet pool exhausted");
            const auto data = pkt.get_data();
            std::printf("P(FULL):%lu:%lx:%lu:%lu:%lu:%u\n",
                (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                static_cast<unsigned long>(pkt.get_can_id()),
                static_cast<unsigned long>(pkt.get_sequence_id()),
                static_cast<unsigned long>(data.empty() ? 0 : data.front()),
                static_cast<unsigned long>(data.empty() ? 0 : data.back()),
                (unsigned int)data.size());
            return;
        }

        *to_send = pkt;
        if (!enqueue_radio_transmit_packet(to_send, send_wait)) {
            _pending_ack_seq_ids[can_id]->store(NO_PENDING_ACK_SEQUENCE_ID);
            ESP_LOGE(TAG, "Failed to push packet to radio transmit queue");
            const auto data = pkt.get_data();
            std::printf("P(FULL):%lu:%lx:%lu:%lu:%lu:%u\n",
                (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                static_cast<unsigned long>(pkt.get_can_id()),
                static_cast<unsigned long>(pkt.get_sequence_id()),
                static_cast<unsigned long>(data.empty() ? 0 : data.front()),
                static_cast<unsigned long>(data.empty() ? 0 : data.back()),
                (unsigned int)data.size());
            return;
        }

        // Wait for ACK semaphore
        uint32_t timeout_ms = PACKET_DELIVERY_TIMEOUT_MIN_MS + (rand() % (PACKET_DELIVERY_TIMEOUT_MAX_MS - PACKET_DELIVERY_TIMEOUT_MIN_MS));
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) > 0) {
            _pending_ack_seq_ids[can_id]->store(NO_PENDING_ACK_SEQUENCE_ID);
            // ACK received!
            return;
        }
    }

    _pending_ack_seq_ids[can_id]->store(NO_PENDING_ACK_SEQUENCE_ID);

    const auto data = pkt.get_data();
    std::printf("P(FAIL):%lu:%lx:%lu:%lu:%lu:%u\n",
                (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()),
                static_cast<unsigned long>(pkt.get_can_id()),
                static_cast<unsigned long>(pkt.get_sequence_id()),
                static_cast<unsigned long>(data.empty() ? 0 : data.front()),
                static_cast<unsigned long>(data.empty() ? 0 : data.back()),
                (unsigned int)data.size());
}

void Transceiver::on_control_packet(const Packet& packet) {
    const auto data = packet.get_data();
    if (data.size() < 2) return;

    uint32_t target_can_id = data[0];
    uint32_t target_seq_id = data[1]; // We could use this for stricter matching if needed

    auto it = _retry_task_handles.find(target_can_id);
    if (it != _retry_task_handles.end() && it->second != nullptr &&
        _pending_ack_seq_ids[target_can_id]->load() == target_seq_id) {
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

    Packet* to_send = acquire_packet(0);
    if (to_send == nullptr) {
        ESP_LOGE(TAG, "Failed to send ACK: send packet pool exhausted");
        return;
    }

    *to_send = ack_pkt;
    if (!enqueue_radio_transmit_packet(to_send, 0)) {
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
