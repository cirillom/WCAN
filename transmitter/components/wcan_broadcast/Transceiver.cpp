#include "Transceiver.hpp"

#include <cstring>
#include <algorithm>
#include <cstdio>
#include <new>

namespace wcan {

static const char* TAG = "WCAN_BCAST";

Transceiver::~Transceiver() {
    stop(100);
}

void Transceiver::stop(uint32_t timeout_ms) {
    _stopping = true;
    stop_retry_tasks(timeout_ms);
    TransceiverBase::stop(timeout_ms);
    delete_ack_queues();
}

bool Transceiver::init() {
    if (!TransceiverBase::init()) return false;

    // Create CONTROL_ID ring for ACK packets
    auto& ctrl_ring = _tx_rings[CONTROL_ID];
    for (auto& slot : ctrl_ring.data) {
        slot.init_for_ring(_mac_addr, CONTROL_ID);
        slot.clear();
    }

    for (CANId_t can_id : _tx_can_ids) {
        _ack_result_queues[can_id] = xQueueCreate(ACK_RESULT_QUEUE_SIZE, sizeof(bool));
        if (!_ack_result_queues[can_id]) {
            ESP_LOGE(TAG, "Failed to create ACK result queue for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            return false;
        }

        _pending_ack_seq_ids[can_id] = std::make_shared<std::atomic<uint32_t>>(NO_PENDING_ACK_SEQUENCE_ID);
        _retry_task_done[can_id] = false;

        auto* ctx = new (std::nothrow) std::pair<Transceiver*, CANId_t>(this, can_id);
        if (ctx == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate retry task context for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            _retry_task_done[can_id] = true;
            return false;
        }

        char name[32];
        std::snprintf(name, sizeof(name), "wcan_retry_%lu", static_cast<unsigned long>(can_id));
        if (xTaskCreate(retry_task_wrapper, name, RETRY_PROCESSING_TASK_STACK_SIZE, ctx, RETRY_PROCESSING_TASK_PRIORITY, &(_retry_task_handles[can_id])) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create retry task for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            delete ctx;
            _retry_task_done[can_id] = true;
            return false;
        }
    }

    return true;
}

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

void Transceiver::dispatch_batch(CANId_t can_id) {
    if (can_id == CONTROL_ID) {
        auto it = _tx_rings.find(CONTROL_ID);
        if (it == _tx_rings.end()) return;
        auto& ring = it->second;
        Packet* pkt_ptr = &ring.read_head();
        if (xQueueSend(_radio_transmit_queue, &pkt_ptr, 0) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send ACK");
            const auto data = pkt_ptr->get_data();
            if (data.size() >= 2) {
                _deduplicator.forget(data[0], data[1]);
            }
            ring.read_head().clear();
            ring.pop();
        }
        return;
    }

    auto it = _retry_task_handles.find(can_id);
    if (it != _retry_task_handles.end() && it->second != nullptr) {
        xTaskNotify(it->second, NOTIFY_BIT_NEW_DATA, eSetBits);
    }
}

void Transceiver::retry_processing_task(CANId_t can_id) {
    auto ring_it = _tx_rings.find(can_id);
    if (ring_it == _tx_rings.end()) {
        _retry_task_done[can_id] = true;
        vTaskDelete(nullptr);
        return;
    }
    auto& ring = ring_it->second;

    while (true) {
        if (is_stopping() && ring.is_empty()) break;

        if (ring.is_empty()) {
            xTaskNotifyWait(~NOTIFY_BIT_NEW_DATA, ULONG_MAX, NULL, pdMS_TO_TICKS(10)); //anything can trigger, but it'll only matter if the !ring.is_empty()
            continue; 
        }

        Packet& pkt = ring.read_head();
        _pending_ack_seq_ids[can_id]->store(pkt.get_sequence_id());
        bool delivered = false;

        xQueueReset(_ack_result_queues[can_id]);

        for (size_t i = 0; i < PACKET_DELIVERY_ATTEMPTS; ++i) {
            xTaskNotifyWait(NOTIFY_BIT_TX_DONE | NOTIFY_BIT_TX_SUCCESS, 0, NULL, 0);

            Packet* pkt_ptr = &pkt;
            if (xQueueSend(_radio_transmit_queue, &pkt_ptr, is_stopping() ? 0 : portMAX_DELAY) != pdTRUE) {
                break;
            }

            bool tx_success = false;
            
            TickType_t ticks_to_wait = pdMS_TO_TICKS(RADIO_TIMEOUT_MS + 50);
            TimeOut_t timeout_state;
            vTaskSetTimeOutState(&timeout_state);

            while (true) {
                uint32_t tx_notified = 0;
                if (xTaskNotifyWait(0, NOTIFY_BIT_TX_DONE | NOTIFY_BIT_TX_SUCCESS, &tx_notified, ticks_to_wait) == pdTRUE) {
                    if (tx_notified & NOTIFY_BIT_TX_DONE) {
                        tx_success = (tx_notified & NOTIFY_BIT_TX_SUCCESS) != 0;
                        break;
                    }
                }
                
                //because NOTIFY_BIT_NEW_DATA can cause the task to unblock and reset the timeout, we need to check if we're actually out of time here instead of just assuming a timeout occurred
                if (xTaskCheckForTimeOut(&timeout_state, &ticks_to_wait) != pdFALSE)
                    break;
            }

            if (tx_success) {
                bool ack_success = false;
                uint32_t timeout_ms = PACKET_DELIVERY_TIMEOUT_MIN_MS + 
                    (rand() % (PACKET_DELIVERY_TIMEOUT_MAX_MS - PACKET_DELIVERY_TIMEOUT_MIN_MS));
                
                if (xQueueReceive(_ack_result_queues[can_id], &ack_success, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && ack_success) {
                    delivered = true;
                    break; // Successfully delivered, exit retry loop
                }
            }
        }

        _pending_ack_seq_ids[can_id]->store(NO_PENDING_ACK_SEQUENCE_ID);
        
        if (!delivered) {
            const auto data = pkt.get_data();
            std::printf("P(FAIL):%lu:%lx:%lu:%lu:%lu:%u\n",
                static_cast<unsigned long>(pdTICKS_TO_MS(xTaskGetTickCount())),
                static_cast<unsigned long>(pkt.get_can_id()),
                static_cast<unsigned long>(pkt.get_sequence_id()),
                static_cast<unsigned long>(data.empty() ? 0 : data.front()),
                static_cast<unsigned long>(data.empty() ? 0 : data.back()),
                static_cast<unsigned int>(data.size()));
        }
        
        ring.read_head().clear();
        ring.pop();
    }

    _retry_task_done[can_id] = true;
    vTaskDelete(nullptr);
}

void Transceiver::on_control_packet(const Packet& packet) {
    const auto data = packet.get_data();
    if (data.size() < 2) return;

    uint32_t target_can_id = data[0];
    uint32_t target_seq_id = data[1];

    auto it = _pending_ack_seq_ids.find(target_can_id);
    if (it != _pending_ack_seq_ids.end() && it->second->load() == target_seq_id) {
        bool success = true;
        auto q_it = _ack_result_queues.find(target_can_id);
        if (q_it != _ack_result_queues.end()) {
            xQueueSend(q_it->second, &success, 0);
        }
    }
}

void Transceiver::on_data_packet(const Packet& packet) {
    if (is_stopping()) return;

    const auto& dest_mac = packet.get_source_mac_addr();
    if (!add_peer(dest_mac.data())) {
        ESP_LOGE(TAG, "Failed to add ACK peer");
        return;
    }

    auto ring_it = _tx_rings.find(CONTROL_ID);
    if (ring_it == _tx_rings.end()) return;
    auto& ring = ring_it->second;

    if (ring.is_full()) {
        ESP_LOGE(TAG, "Failed to send ACK: CONTROL ring full");
        return;
    }

    uint32_t mac_part1, mac_part2;
    std::memcpy(&mac_part1, dest_mac.data(), 4);
    std::memcpy(&mac_part2, dest_mac.data() + 4, 2);

    auto& pkt = ring.write_head();
    pkt.clear();
    pkt.add_data_point(packet.get_can_id());
    pkt.add_data_point(packet.get_sequence_id());
    pkt.add_data_point(mac_part1);
    pkt.add_data_point(mac_part2);

    //ideally we would also use send_data(CONTROL_ID, ...) here, but that would start a linger timer which we don't want for ACKs

    finish_batch(CONTROL_ID);
}

void Transceiver::on_radio_send(CANId_t can_id, bool success) {
    if (can_id == CONTROL_ID) {
        auto it = _tx_rings.find(CONTROL_ID);
        if (it != _tx_rings.end()) {
            auto& ring = it->second;
            if (!ring.is_empty()) {
                ring.read_head().clear();
                ring.pop();
            }
        }
        return;
    }

    auto it = _retry_task_handles.find(can_id);
    if (it != _retry_task_handles.end() && it->second != nullptr) {
        uint32_t bits = NOTIFY_BIT_TX_DONE;
        if (success) {
            bits |= NOTIFY_BIT_TX_SUCCESS;
        }
        xTaskNotify(it->second, bits, eSetBits);
    }
}

void Transceiver::stop_retry_tasks(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    bool all_done = false;
    while (!all_done) {
        all_done = true;
        for (const auto& entry : _retry_task_done) {
            if (!entry.second) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        if (timeout_ms == 0 || static_cast<int32_t>(xTaskGetTickCount() - start) >= static_cast<int32_t>(timeout_ticks)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (auto& entry : _retry_task_handles) {
        if (!_retry_task_done[entry.first] && entry.second) {
            vTaskDelete(entry.second);
            _retry_task_done[entry.first] = true;
        }
    }
}

void Transceiver::delete_ack_queues() {
    for (auto& entry : _ack_result_queues) {
        if (entry.second) {
            vQueueDelete(entry.second);
            entry.second = nullptr;
        }
    }
}

bool Transceiver::add_peer(const uint8_t* mac_addr) {
    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
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
