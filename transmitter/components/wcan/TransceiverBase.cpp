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
    _packet_pool_size = RADIO_TRANSMIT_QUEUE_SIZE + (BATCH_QUEUE_SIZE * std::max(_tx_can_ids.size(), _rx_can_ids.size()));
    _packet_pool = new (std::nothrow) Packet[_packet_pool_size];
    _rx_packet_pool = new (std::nothrow) EspNowPacket[RX_PACKET_POOL_SIZE];
    if (_packet_pool == nullptr || _rx_packet_pool == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate packet pools");
        return false;
    }

    _free_packets = xQueueCreate(_packet_pool_size, sizeof(Packet*));
    _radio_transmit_queue = xQueueCreate(RADIO_TRANSMIT_QUEUE_SIZE, sizeof(Packet*));
    _free_rx_packets = xQueueCreate(RX_PACKET_POOL_SIZE, sizeof(EspNowPacket*));
    _recv_queue = xQueueCreate(RECV_QUEUE_SIZE, sizeof(EspNowPacket*));
    if (!_free_packets || !_radio_transmit_queue || !_free_rx_packets || !_recv_queue) {
        ESP_LOGE(TAG, "Failed to create packet pool queues");
        return false;
    }

    for (size_t i = 0; i < _packet_pool_size; ++i) {
        Packet* packet = &_packet_pool[i];
        packet->clear();
        if (xQueueSend(_free_packets, &packet, 0) != pdTRUE) {
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
            QueueHandle_t batch_q = xQueueCreate(BATCH_QUEUE_SIZE, sizeof(Packet*));
            if (!q || !batch_q) return false;
            _can_data_queues.emplace(_tx_can_ids[i], q);
            _batch_queues.emplace(_tx_can_ids[i], batch_q);
            _batch_task_handles.emplace(_tx_can_ids[i], (TaskHandle_t)nullptr);
            _batch_task_done.emplace(_tx_can_ids[i], false);
            _retry_task_handles.emplace(_tx_can_ids[i], (TaskHandle_t)nullptr);
            _retry_task_done.emplace(_tx_can_ids[i], false);
            _pending_ack_seq_ids.emplace(_tx_can_ids[i], std::make_shared<std::atomic<uint32_t>>(NO_PENDING_ACK_SEQUENCE_ID));
        }
    }

    if (_stats) _stats->configure_tx_ids(_tx_can_ids);

    _tx_result_queue = xQueueCreate(TX_RESULT_QUEUE_SIZE, sizeof(bool));
    if (!_tx_result_queue) return false;

    if (!setup_esp_now()) return false;
    if (!start_tasks()) {
        stop(0);
        return false;
    }
    return true;
}

Packet* TransceiverBase::acquire_packet(TickType_t wait_ticks) {
    if (_free_packets == nullptr) return nullptr;
    Packet* packet = nullptr;
    if (xQueueReceive(_free_packets, &packet, wait_ticks) != pdTRUE || packet == nullptr) {
        return nullptr;
    }
    packet->clear();
    return packet;
}

bool TransceiverBase::enqueue_radio_transmit_packet(Packet* packet, TickType_t wait_ticks) {
    if (packet == nullptr) return false;
    if (_radio_transmit_queue != nullptr && xQueueSend(_radio_transmit_queue, &packet, wait_ticks) == pdTRUE) {
        return true;
    }
    release_packet(packet);
    return false;
}

void TransceiverBase::release_packet(Packet* packet) {
    if (packet == nullptr || _free_packets == nullptr) return;
    packet->clear();
    (void)xQueueSend(_free_packets, &packet, 0);
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

bool TransceiverBase::start_tasks() {
    _send_task_done = false;
    _recv_task_done = false;

    if (xTaskCreate(send_task_wrapper, "wcan_send", SEND_PROCESSING_TASK_STACK_SIZE, this, SEND_PROCESSING_TASK_PRIORITY, &_send_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create send task");
        _send_task_done = true;
        return false;
    }
    if (xTaskCreate(recv_task_wrapper, "wcan_recv", RECV_PROCESSING_TASK_STACK_SIZE, this, RECV_PROCESSING_TASK_PRIORITY, &_recv_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create receive task");
        _recv_task_done = true;
        return false;
    }

    for (size_t i = 0; i < _tx_can_ids.size(); i++) {
        const CANId_t can_id = _tx_can_ids[i];
        _batch_task_done[can_id] = false;
        _retry_task_done[can_id] = false;

        auto* retry_ctx = new (std::nothrow) std::pair<TransceiverBase*, CANId_t>(this, can_id);
        if (retry_ctx == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate retry task context for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            _retry_task_done[can_id] = true;
            return false;
        }
        char retry_name[32]; std::snprintf(retry_name, sizeof(retry_name), "wcan_retry_%lu", static_cast<unsigned long>(can_id));
        if (xTaskCreate(retry_task_wrapper, retry_name, RETRY_PROCESSING_TASK_STACK_SIZE, retry_ctx, BATCH_PROCESSING_TASK_PRIORITY, &(_retry_task_handles[can_id])) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create retry task for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            delete retry_ctx;
            _retry_task_done[can_id] = true;
            return false;
        }

        auto* batch_ctx = new (std::nothrow) std::pair<TransceiverBase*, CANId_t>(this, can_id);
        if (batch_ctx == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate batch task context for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            _batch_task_done[can_id] = true;
            return false;
        }
        char batch_name[32]; std::snprintf(batch_name, sizeof(batch_name), "wcan_batch_%lu", static_cast<unsigned long>(can_id));
        if (xTaskCreate(batch_task_wrapper, batch_name, BATCH_PROCESSING_TASK_STACK_SIZE, batch_ctx, BATCH_PROCESSING_TASK_PRIORITY, &(_batch_task_handles[can_id])) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create batch task for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            delete batch_ctx;
            _batch_task_done[can_id] = true;
            return false;
        }
    }

    return true;
}

void TransceiverBase::send_processing_task() {
    uint8_t encoded[ESP_NOW_MAX_DATA_LENGTH];

    while (true) {
        bool retries_done = true;
        for (const auto& entry : _retry_task_done) {
            if (!entry.second) {
                retries_done = false;
                break;
            }
        }
        if (_stopping && retries_done && _recv_task_done && (_radio_transmit_queue == nullptr || uxQueueMessagesWaiting(_radio_transmit_queue) == 0)) {
            break;
        }

        Packet* pkt = nullptr;
        if (_radio_transmit_queue != nullptr && xQueueReceive(_radio_transmit_queue, &pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (pkt == nullptr) continue;

            size_t encoded_size = 0;
            if (!pkt->encode(encoded, sizeof(encoded), &encoded_size)) {
                release_packet(pkt);
                continue;
            }

            // Determine destination using the virtual hook
            const uint8_t* dest_mac = prepare_send_mac(*pkt);

            xQueueReset(_tx_result_queue);
            const int64_t send_start_us = stats().now_us();

            esp_err_t err = esp_now_send(dest_mac, encoded, encoded_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_now_send fail: %s", esp_err_to_name(err));
                release_packet(pkt);
                continue;
            }

            bool success = false;
            if (xQueueReceive(_tx_result_queue, &success, pdMS_TO_TICKS(RADIO_TIMEOUT_MS)) == pdTRUE) {
                const int64_t send_end_us = stats().now_us();
                stats().record_airtime(static_cast<uint32_t>(std::max<int64_t>(0, send_end_us - send_start_us)));
                if (!success && dest_mac != nullptr) {
                    std::printf("Radio send to %02x:%02x:%02x:%02x:%02x:%02x failed\n",
                        dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5]);

                    const auto data = pkt->get_data();
                    _deduplicator.forget(data[0], data[1]);
                }
            } else {
                std::printf("Radio hardware timeout (50ms) to %02x:%02x:%02x:%02x:%02x:%02x\n",
                    dest_mac ? dest_mac[0] : 0, dest_mac ? dest_mac[1] : 0, dest_mac ? dest_mac[2] : 0,
                    dest_mac ? dest_mac[3] : 0, dest_mac ? dest_mac[4] : 0, dest_mac ? dest_mac[5] : 0);
            }

            release_packet(pkt);
        }
    }
    _send_task_done = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::recv_processing_task() {
    while (true) {
        bool retries_done = true;
        for (const auto& entry : _retry_task_done) {
            if (!entry.second) {
                retries_done = false;
                break;
            }
        }
        const bool send_empty = _radio_transmit_queue == nullptr || uxQueueMessagesWaiting(_radio_transmit_queue) == 0;
        const bool recv_empty = _recv_queue == nullptr || uxQueueMessagesWaiting(_recv_queue) == 0;
        if (_stopping && retries_done && send_empty && recv_empty) {
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
    auto batch_it = _batch_queues.find(can_id);
    if (it == _can_data_queues.end() || batch_it == _batch_queues.end()) {
        _batch_task_done[can_id] = true;
        vTaskDelete(nullptr);
        return;
    }
    QueueHandle_t q = it->second;
    QueueHandle_t batch_q = batch_it->second;

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
        Packet* batch_pkt = nullptr;
        while (!_stopping) {
            batch_pkt = acquire_packet(pdMS_TO_TICKS(10));
            if (batch_pkt != nullptr) break;
        }
        if (batch_pkt == nullptr) {
            continue;
        }

        *batch_pkt = pkt;
        stats().record_batch(*batch_pkt, ready_us);

        bool enqueued = false;
        while (!_stopping) {
            if (batch_q != nullptr && xQueueSend(batch_q, &batch_pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
                enqueued = true;
                break;
            }
        }

        if (!enqueued) {
            release_packet(batch_pkt);
            continue;
        }

    }
    _batch_task_done[can_id] = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::retry_processing_task(CANId_t can_id) {
    auto it = _batch_queues.find(can_id);
    if (it == _batch_queues.end()) {
        _retry_task_done[can_id] = true;
        vTaskDelete(nullptr);
        return;
    }
    QueueHandle_t q = it->second;

    while (true) {
        const bool batch_done = _batch_task_done[can_id];
        const bool batch_empty = q == nullptr || uxQueueMessagesWaiting(q) == 0;
        if (_stopping && batch_done && batch_empty) {
            break;
        }

        Packet* pkt = nullptr;
        if (q != nullptr && xQueueReceive(q, &pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (pkt == nullptr) continue;

            const int64_t dispatch_start_us = stats().now_us();
            dispatch_packet(*pkt, can_id);
            const int64_t dispatch_end_us = stats().now_us();
            const uint32_t dispatch_us = static_cast<uint32_t>(std::max<int64_t>(0, dispatch_end_us - dispatch_start_us));
            stats().record_batch_dispatch(*pkt, dispatch_start_us, dispatch_us);
            release_packet(pkt);
        }
    }

    _retry_task_done[can_id] = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (!s_instance || s_instance->_tx_result_queue == nullptr) return;
    const bool success = status == ESP_NOW_SEND_SUCCESS;
    s_instance->on_hardware_tx_status(mac, success);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)xQueueSendFromISR(s_instance->_tx_result_queue, &success, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void TransceiverBase::esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_instance || !info->src_addr || !data || len < Packet::HEADER_SIZE) return;
    if (len > ESP_NOW_MAX_DATA_LENGTH) return;

    CANId_t can_id = EspNowPacket::extract_can_id(data);
    if (s_instance->_stopping && can_id != CONTROL_ID) return;

    if (!s_instance->should_accept(can_id)) return;

    /* Use ISR-safe queue APIs: these callbacks may run in ISR/context
       where standard xQueue* calls are not allowed. */
    if (s_instance->_free_rx_packets == nullptr) return;
    EspNowPacket* raw_pkt = nullptr;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueReceiveFromISR(s_instance->_free_rx_packets, &raw_pkt, &xHigherPriorityTaskWoken) != pdTRUE || raw_pkt == nullptr) {
        return;
    }

    std::memcpy(raw_pkt->src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->des_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->payload, data, static_cast<size_t>(len));
    raw_pkt->payload_len = static_cast<size_t>(len);

    if (s_instance->_recv_queue == nullptr || xQueueSendFromISR(s_instance->_recv_queue, &raw_pkt, &xHigherPriorityTaskWoken) != pdTRUE) {
        /* return packet to free pool */
        (void)xQueueSendFromISR(s_instance->_free_rx_packets, &raw_pkt, &xHigherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

bool TransceiverBase::queues_drained() const {
    if (_radio_transmit_queue != nullptr && uxQueueMessagesWaiting(_radio_transmit_queue) > 0) return false;
    if (_recv_queue != nullptr && uxQueueMessagesWaiting(_recv_queue) > 0) return false;
    for (const auto& entry : _can_data_queues) {
        if (entry.second != nullptr && uxQueueMessagesWaiting(entry.second) > 0) return false;
    }
    for (const auto& entry : _batch_queues) {
        if (entry.second != nullptr && uxQueueMessagesWaiting(entry.second) > 0) return false;
    }
    if (!_send_task_done || !_recv_task_done) return false;
    for (const auto& entry : _batch_task_done) {
        if (!entry.second) return false;
    }
    for (const auto& entry : _retry_task_done) {
        if (!entry.second) return false;
    }
    return true;
}

void TransceiverBase::delete_queues() {
    if (_radio_transmit_queue) {
        Packet* packet = nullptr;
        while (xQueueReceive(_radio_transmit_queue, &packet, 0) == pdTRUE) {
            release_packet(packet);
        }
        vQueueDelete(_radio_transmit_queue);
        _radio_transmit_queue = nullptr;
    }
    if (_recv_queue) {
        vQueueDelete(_recv_queue);
        _recv_queue = nullptr;
    }
    for (auto& entry : _batch_queues) {
        if (entry.second) {
            Packet* packet = nullptr;
            while (xQueueReceive(entry.second, &packet, 0) == pdTRUE) {
                release_packet(packet);
            }
            vQueueDelete(entry.second);
            entry.second = nullptr;
        }
    }
    if (_free_packets) {
        vQueueDelete(_free_packets);
        _free_packets = nullptr;
    }
    if (_free_rx_packets) {
        vQueueDelete(_free_rx_packets);
        _free_rx_packets = nullptr;
    }
    delete[] _packet_pool;
    _packet_pool = nullptr;
    _packet_pool_size = 0;
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
    if (_stopping && _radio_transmit_queue == nullptr && _recv_queue == nullptr) {
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
    for (auto& entry : _retry_task_handles) {
        if (!_retry_task_done[entry.first] && entry.second) {
            vTaskDelete(entry.second);
            _retry_task_done[entry.first] = true;
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
