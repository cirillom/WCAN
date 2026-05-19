#include "TransceiverBase.hpp"

#include <cstring>
#include <algorithm>
#include <cstdio>
#include <new>

namespace wcan {

static const char* TAG = "WCAN_BASE";

TransceiverBase* TransceiverBase::s_instance = nullptr;

TransceiverBase::~TransceiverBase() {
    stop(100);
    delete_queues();
}

TransceiverBase::TransceiverBase(std::vector<uint32_t> rx_can_ids, std::vector<uint32_t> tx_can_ids, uint32_t linger_ms, bool filtering_enabled)
    : _stats(make_stats()),
      _rx_can_ids(std::move(rx_can_ids)),
      _tx_can_ids(std::move(tx_can_ids)),
      _linger_ms(linger_ms),
      _filtering_enabled(filtering_enabled) {}

bool TransceiverBase::init_pools() {
    _send_packet_pool = new (std::nothrow) Packet[SEND_PACKET_POOL_SIZE];
    _rx_packet_pool = new (std::nothrow) EspNowPacket[RX_PACKET_POOL_SIZE];
    if (_send_packet_pool == nullptr || _rx_packet_pool == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate packet pools");
        return false;
    }

    _free_send_packets = xQueueCreate(SEND_PACKET_POOL_SIZE, sizeof(Packet*));
    _send_queue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(Packet*));
    _free_rx_packets = xQueueCreate(RX_PACKET_POOL_SIZE, sizeof(EspNowPacket*));
    _recv_queue = xQueueCreate(RECV_QUEUE_SIZE, sizeof(EspNowPacket*));
    if (!_free_send_packets || !_send_queue || !_free_rx_packets || !_recv_queue) {
        ESP_LOGE(TAG, "Failed to create packet pool queues");
        return false;
    }

    for (size_t i = 0; i < SEND_PACKET_POOL_SIZE; ++i) {
        Packet* packet = &_send_packet_pool[i];
        packet->clear();
        if (xQueueSend(_free_send_packets, &packet, 0) != pdTRUE) {
            return false;
        }
    }

    for (size_t i = 0; i < RX_PACKET_POOL_SIZE; ++i) {
        EspNowPacket* packet = &_rx_packet_pool[i];
        packet->payload_len = 0;
        if (xQueueSend(_free_rx_packets, &packet, 0) != pdTRUE) {
            return false;
        }
    }

    return true;
}

bool TransceiverBase::init() {
    if (s_instance != nullptr) return false;

    ESP_ERROR_CHECK(esp_read_mac(_mac_addr.data(), MAC_TYPE));

    if (!init_pools()) return false;

    const size_t tx_count = _tx_can_ids.size();
    if (tx_count > 0) {
        for (size_t i = 0; i < tx_count; i++) {
            QueueHandle_t q = xQueueCreate(CAN_DATA_QUEUE_SIZE, sizeof(DataPoint_t));
            if (!q) return false;
            _can_data_queues.emplace(_tx_can_ids[i], q);
            _batch_task_handles.emplace(_tx_can_ids[i], (TaskHandle_t)nullptr);
            _batch_task_done.emplace(_tx_can_ids[i], false);
            _pending_ack_seq_ids.emplace(_tx_can_ids[i], NO_PENDING_ACK_SEQUENCE_ID);
        }
    }

    _tx_result_queue = xQueueCreate(TX_RESULT_QUEUE_SIZE, sizeof(bool));
    if (!_tx_result_queue) return false;

    if (!setup_esp_now()) return false;
    start_tasks();
    return true;
}

Packet* TransceiverBase::acquire_send_packet(TickType_t wait_ticks) {
    if (_free_send_packets == nullptr) return nullptr;
    Packet* packet = nullptr;
    if (xQueueReceive(_free_send_packets, &packet, wait_ticks) != pdTRUE || packet == nullptr) {
        return nullptr;
    }
    packet->clear();
    return packet;
}

bool TransceiverBase::enqueue_send_packet(Packet* packet, TickType_t wait_ticks) {
    if (packet == nullptr) return false;
    if (_send_queue != nullptr && xQueueSend(_send_queue, &packet, wait_ticks) == pdTRUE) {
        return true;
    }
    release_send_packet(packet);
    return false;
}

void TransceiverBase::release_send_packet(Packet* packet) {
    if (packet == nullptr || _free_send_packets == nullptr) return;
    packet->clear();
    (void)xQueueSend(_free_send_packets, &packet, 0);
}

EspNowPacket* TransceiverBase::acquire_rx_packet() {
    if (_free_rx_packets == nullptr) return nullptr;
    EspNowPacket* packet = nullptr;
    if (xQueueReceive(_free_rx_packets, &packet, 0) != pdTRUE || packet == nullptr) {
        return nullptr;
    }
    packet->payload_len = 0;
    return packet;
}

void TransceiverBase::release_rx_packet(EspNowPacket* packet) {
    if (packet == nullptr || _free_rx_packets == nullptr) return;
    packet->payload_len = 0;
    (void)xQueueSend(_free_rx_packets, &packet, 0);
}

bool TransceiverBase::send_data(CANId_t can_id, DataPoint_t data) {
    if (_stopping) return false;
    auto it = _can_data_queues.find(can_id);
    if (it == _can_data_queues.end() || it->second == nullptr) return false;
    return xQueueSend(it->second, &data, 0) == pdTRUE;
}

bool TransceiverBase::setup_esp_now() {
    if (esp_now_init() != ESP_OK) return false;

    s_instance = this;
    esp_now_register_send_cb(esp_now_send_cb);
    esp_now_register_recv_cb(esp_now_recv_cb);

    return this->add_peer(Packet::BROADCAST_MAC.data());
}

void TransceiverBase::start_tasks() {
    _send_task_done = false;
    _recv_task_done = false;
    xTaskCreate(send_task_wrapper, "wcan_send", SEND_PROCESSING_TASK_STACK_SIZE, this, SEND_PROCESSING_TASK_PRIORITY, &_send_task_handle);
    xTaskCreate(recv_task_wrapper, "wcan_recv", RECV_PROCESSING_TASK_STACK_SIZE, this, RECV_PROCESSING_TASK_PRIORITY, &_recv_task_handle);

    for (size_t i = 0; i < _tx_can_ids.size(); i++) {
        _batch_task_done[_tx_can_ids[i]] = false;
        auto* ctx = new std::pair<TransceiverBase*, CANId_t>(this, _tx_can_ids[i]);
        char name[32]; std::snprintf(name, sizeof(name), "wcan_batch_%lu", (unsigned long)_tx_can_ids[i]);
        xTaskCreate(batch_task_wrapper, name, BATCH_PROCESSING_TASK_STACK_SIZE, ctx, BATCH_PROCESSING_TASK_PRIORITY, &(_batch_task_handles[_tx_can_ids[i]]));
    }
}

void TransceiverBase::send_processing_task() {
    uint8_t encoded[ESP_NOW_MAX_DATA_LENGTH];

    while (true) {
        bool batches_done = true;
        for (const auto& entry : _batch_task_done) {
            if (!entry.second) {
                batches_done = false;
                break;
            }
        }
        if (_stopping && batches_done && _recv_task_done && (_send_queue == nullptr || uxQueueMessagesWaiting(_send_queue) == 0)) {
            break;
        }

        Packet* pkt = nullptr;
        if (_send_queue != nullptr && xQueueReceive(_send_queue, &pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (pkt == nullptr) continue;

            size_t encoded_size = 0;
            if (!pkt->encode(encoded, sizeof(encoded), &encoded_size)) {
                release_send_packet(pkt);
                continue;
            }

            // Determine destination using the virtual hook
            const uint8_t* dest_mac = prepare_send_mac(*pkt);

            for (int attempt = 0; attempt < RADIO_MAX_RETRIES; attempt++) {
                xQueueReset(_tx_result_queue);
                const int64_t send_start_us = stats().now_us();

                esp_err_t err = esp_now_send(dest_mac, encoded, encoded_size);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_now_send fail: %s", esp_err_to_name(err));
                    continue;
                }

                bool success = false;
                if (xQueueReceive(_tx_result_queue, &success, pdMS_TO_TICKS(RADIO_TIMEOUT_MS)) == pdTRUE) {
                    const int64_t send_end_us = stats().now_us();
                    stats().record_airtime(static_cast<uint32_t>(std::max<int64_t>(0, send_end_us - send_start_us)));
                    if (success) {
                        break;
                    } else if (dest_mac != nullptr) {
                        std::printf("Radio send to %02x:%02x:%02x:%02x:%02x:%02x failed on attempt %d\n",
                            dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5], attempt + 1);
                    }
                }
            }

            release_send_packet(pkt);
        }
    }
    _send_task_done = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::recv_processing_task() {
    while (true) {
        bool batches_done = true;
        for (const auto& entry : _batch_task_done) {
            if (!entry.second) {
                batches_done = false;
                break;
            }
        }
        const bool send_empty = _send_queue == nullptr || uxQueueMessagesWaiting(_send_queue) == 0;
        const bool recv_empty = _recv_queue == nullptr || uxQueueMessagesWaiting(_recv_queue) == 0;
        if (_stopping && batches_done && send_empty && recv_empty) {
            break;
        }

        EspNowPacket* raw_pkt = nullptr;
        if (_recv_queue != nullptr && xQueueReceive(_recv_queue, &raw_pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (raw_pkt == nullptr) continue;

            Packet pkt;
            const bool decoded = Packet::from_payload(raw_pkt->src_mac, raw_pkt->payload, raw_pkt->payload_len, pkt, raw_pkt->des_mac);
            release_rx_packet(raw_pkt);

            if (!decoded) continue;

            if (_deduplicator.check_and_update(pkt)) continue;

            if (pkt.get_can_id() == CONTROL_ID) {
                on_control_packet(pkt);
            } else {
                on_data_packet(pkt);
                stats().record_rx_packet(pkt);
                if (_recv_callback) _recv_callback(pkt);
            }
        }
    }
    _recv_task_done = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::batch_processing_task(CANId_t can_id) {
    auto it = _can_data_queues.find(can_id);
    if (it == _can_data_queues.end()) return;
    QueueHandle_t q = it->second;

    while (true) {
        if (_stopping && uxQueueMessagesWaiting(q) == 0) {
            break;
        }
        DataPoint_t data;
        if (xQueueReceive(q, &data, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        Packet pkt(_mac_addr, can_id);
        pkt.add_data_point(data);

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(_linger_ms);
        while (true) {
            if (pkt.get_data().size() >= Packet::MAX_DATA_POINTS) break;

            const TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(deadline - now) <= 0) break;

            const TickType_t wait_ticks = _stopping ? 0 : (deadline - now);
            if (xQueueReceive(q, &data, wait_ticks) == pdTRUE) {
                if (!pkt.add_data_point(data)) {
                    ESP_LOGE(TAG, "Failed to add data point to packet");
                    break;
                }
            } else {
                break;
            }
        }
        const int64_t ready_us = stats().now_us();
        // Hand the constructed batch to the Strategy to handle retries/dispatch
        dispatch_packet(pkt, can_id);
        const uint32_t dispatch_us = static_cast<uint32_t>(std::max<int64_t>(0, stats().now_us() - ready_us));
        stats().record_batch(can_id, static_cast<uint32_t>(pkt.get_data().size()), ready_us, dispatch_us);
    }
    _batch_task_done[can_id] = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (!s_instance || s_instance->_tx_result_queue == nullptr) return;
    const bool success = status == ESP_NOW_SEND_SUCCESS;
    s_instance->on_hardware_tx_status(mac, success);
    xQueueSend(s_instance->_tx_result_queue, &success, 0);
}

void TransceiverBase::esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_instance || !info->src_addr || !data || len < Packet::HEADER_SIZE) return;
    if (len > ESP_NOW_MAX_DATA_LENGTH) return;

    CANId_t can_id = EspNowPacket::extract_can_id(data);
    if (s_instance->_stopping && can_id != CONTROL_ID) return;

    if (!s_instance->should_accept(can_id)) return;

    EspNowPacket* raw_pkt = s_instance->acquire_rx_packet();
    if (!raw_pkt) return;

    std::memcpy(raw_pkt->src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->des_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->payload, data, static_cast<size_t>(len));
    raw_pkt->payload_len = static_cast<size_t>(len);

    if (s_instance->_recv_queue == nullptr || xQueueSend(s_instance->_recv_queue, &raw_pkt, 0) != pdTRUE) {
        s_instance->release_rx_packet(raw_pkt);
    }
}

bool TransceiverBase::queues_drained() const {
    if (_send_queue != nullptr && uxQueueMessagesWaiting(_send_queue) > 0) return false;
    if (_recv_queue != nullptr && uxQueueMessagesWaiting(_recv_queue) > 0) return false;
    for (const auto& entry : _can_data_queues) {
        if (entry.second != nullptr && uxQueueMessagesWaiting(entry.second) > 0) return false;
    }
    if (!_send_task_done || !_recv_task_done) return false;
    for (const auto& entry : _batch_task_done) {
        if (!entry.second) return false;
    }
    return true;
}

void TransceiverBase::delete_queues() {
    if (_send_queue) {
        vQueueDelete(_send_queue);
        _send_queue = nullptr;
    }
    if (_recv_queue) {
        vQueueDelete(_recv_queue);
        _recv_queue = nullptr;
    }
    if (_free_send_packets) {
        vQueueDelete(_free_send_packets);
        _free_send_packets = nullptr;
    }
    if (_free_rx_packets) {
        vQueueDelete(_free_rx_packets);
        _free_rx_packets = nullptr;
    }
    delete[] _send_packet_pool;
    _send_packet_pool = nullptr;
    delete[] _rx_packet_pool;
    _rx_packet_pool = nullptr;

    for (auto& entry : _can_data_queues) {
        if (entry.second) {
            vQueueDelete(entry.second);
            entry.second = nullptr;
        }
    }
    if (_tx_result_queue) {
        vQueueDelete(_tx_result_queue);
        _tx_result_queue = nullptr;
    }
}

void TransceiverBase::stop(uint32_t timeout_ms) {
    if (_stopping && _send_queue == nullptr && _recv_queue == nullptr) {
        return;
    }

    _stopping = true;
    _recv_callback = nullptr;

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (!queues_drained()) {
        if (timeout_ms == 0 || static_cast<int32_t>(xTaskGetTickCount() - start) >= static_cast<int32_t>(timeout_ticks)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!_send_task_done && _send_task_handle) {
        vTaskDelete(_send_task_handle);
        _send_task_done = true;
    }
    if (!_recv_task_done && _recv_task_handle) {
        vTaskDelete(_recv_task_handle);
        _recv_task_done = true;
    }
    for (auto& entry : _batch_task_handles) {
        if (!_batch_task_done[entry.first] && entry.second) {
            vTaskDelete(entry.second);
            _batch_task_done[entry.first] = true;
        }
    }

    if (s_instance == this) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        s_instance = nullptr;
    }
    delete_queues();
}

bool TransceiverBase::should_accept(CANId_t can_id) const {
    if (can_id == CONTROL_ID) return true;
    if (!_filtering_enabled) return true;
    return std::find(_rx_can_ids.begin(), _rx_can_ids.end(), can_id) != _rx_can_ids.end();
}
} // namespace wcan
