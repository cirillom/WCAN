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
}

TransceiverBase::TransceiverBase(std::vector<uint32_t> rx_can_ids, std::vector<uint32_t> tx_can_ids, uint32_t linger_ms, bool filtering_enabled)
    : _stats(make_stats()),
      _rx_can_ids(std::move(rx_can_ids)),
      _tx_can_ids(std::move(tx_can_ids)),
      _linger_ms(linger_ms),
      _filtering_enabled(filtering_enabled) {}

bool TransceiverBase::init() {
    if (s_instance != nullptr) return false;

    ESP_ERROR_CHECK(esp_read_mac(_mac_addr.data(), MAC_TYPE));

    _radio_transmit_queue = xQueueCreate(RADIO_TRANSMIT_QUEUE_SIZE, sizeof(Packet*));
    _tx_result_queue = xQueueCreate(TX_RESULT_QUEUE_SIZE, sizeof(bool));
    if (!_radio_transmit_queue || !_tx_result_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        return false;
    }

    // Initialize TX ring buffers (one per CAN ID)
    _linger_timer_contexts.reserve(_tx_can_ids.size());
    for (CANId_t can_id : _tx_can_ids) {
        auto& ring = _tx_rings[can_id];
        for (auto& slot : ring.data) {
            slot.init_for_ring(_mac_addr, can_id);
            slot.clear();
        }

        _linger_timer_contexts.push_back({this, can_id});
        auto& ctx = _linger_timer_contexts.back();

        esp_timer_create_args_t args = {};
        args.callback = linger_timer_callback;
        args.arg = &ctx;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "wcan_linger";

        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create linger timer for CAN ID 0x%lx", static_cast<unsigned long>(can_id));
            return false;
        }
        _linger_timers[can_id] = timer;
    }

    if (_stats) _stats->configure_tx_ids(_tx_can_ids);

    if (!setup_esp_now()) return false;
    if (!start_tasks()) {
        stop(0);
        return false;
    }
    return true;
}

bool TransceiverBase::send_data(CANId_t can_id, DataPoint_t data) {
    if (_stopping) return false;
    auto it = _tx_rings.find(can_id);
    if (it == _tx_rings.end()) return false;
    auto& ring = it->second;
    if (ring.is_full()) {
        stats().record_sensor_send_failure(can_id, data);
        return false;
    }

    auto& pkt = ring.write_head();
    pkt.add_data_point(data);

    if (pkt.get_data_count() >= Packet::MAX_DATA_POINTS) {
        auto timer_it = _linger_timers.find(can_id);
        if (timer_it != _linger_timers.end()) {
            esp_timer_stop(timer_it->second);
        }
        finish_batch(can_id);
    } else if (pkt.get_data_count() == 1) {
        if (_linger_ms == 0) {
            finish_batch(can_id);
        } else {
            auto timer_it = _linger_timers.find(can_id);
            if (timer_it != _linger_timers.end()) {
                esp_timer_start_once(timer_it->second, _linger_ms * 1000);
            }
        }
    }
    return true;
}

void TransceiverBase::finish_batch(CANId_t can_id) {
    auto it = _tx_rings.find(can_id);
    if (it == _tx_rings.end()) return;
    auto& ring = it->second;
    if (ring.write_head().get_data_count() == 0) return;

    ring.write_head().assign_new_sequence_id();

    const int64_t ready_us = stats().now_us();
    ring.write_head().set_ready_us(ready_us);
    stats().record_batch(ring.write_head(), ready_us);

    ring.push();
    dispatch_batch(can_id);
}

void TransceiverBase::linger_timer_callback(void* arg) {
    auto* ctx = static_cast<std::pair<TransceiverBase*, CANId_t>*>(arg);
    ctx->first->finish_batch(ctx->second);
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

    return true;
}

void TransceiverBase::send_processing_task() {
    uint8_t encoded[ESP_NOW_MAX_DATA_LENGTH];

    while (true) {
        if (_stopping && (_radio_transmit_queue == nullptr || uxQueueMessagesWaiting(_radio_transmit_queue) == 0)) {
            break;
        }

        Packet* pkt = nullptr;
        if (_radio_transmit_queue != nullptr && xQueueReceive(_radio_transmit_queue, &pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (pkt == nullptr) continue;

            size_t encoded_size = 0;
            if (!pkt->encode(encoded, sizeof(encoded), &encoded_size)) {
                on_radio_send(pkt->get_can_id(), false);
                continue;
            }

            const uint8_t* dest_mac = prepare_send_mac(*pkt);

            xQueueReset(_tx_result_queue);
            const int64_t send_start_us = stats().now_us();

            esp_err_t err = esp_now_send(dest_mac, encoded, encoded_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_now_send fail: %s", esp_err_to_name(err));
                on_radio_send(pkt->get_can_id(), false);
                continue;
            }

            bool success = false;
            if (xQueueReceive(_tx_result_queue, &success, pdMS_TO_TICKS(RADIO_TIMEOUT_MS)) == pdTRUE) {
                const int64_t send_end_us = stats().now_us();
                stats().record_airtime(static_cast<uint32_t>(std::max<int64_t>(0, send_end_us - send_start_us)));

                const int64_t dispatch_end_us = stats().now_us();
                const uint32_t dispatch_us = static_cast<uint32_t>(std::max<int64_t>(0, dispatch_end_us - send_start_us));
                stats().record_batch_dispatch(*pkt, send_start_us, dispatch_us);

                on_radio_send(pkt->get_can_id(), success);
            } else {
                std::printf("Radio hardware timeout (%lums) to %02x:%02x:%02x:%02x:%02x:%02x\n",
                    static_cast<unsigned long>(RADIO_TIMEOUT_MS),
                    dest_mac ? dest_mac[0] : 0, dest_mac ? dest_mac[1] : 0, dest_mac ? dest_mac[2] : 0,
                    dest_mac ? dest_mac[3] : 0, dest_mac ? dest_mac[4] : 0, dest_mac ? dest_mac[5] : 0);
                on_radio_send(pkt->get_can_id(), false);
            }
        }
    }
    _send_task_done = true;
    vTaskDelete(nullptr);
}

void TransceiverBase::recv_processing_task() {
    while (true) {
        if (_stopping && _rx_packets.is_empty()) break;

        uint32_t notified_value = 0;
        xTaskNotifyWait(0x00, NOTIFY_BIT_NEW_DATA, &notified_value, pdMS_TO_TICKS(10));
        if ((notified_value & NOTIFY_BIT_NEW_DATA) == 0) continue;

        while (!_rx_packets.is_empty()) {
            EspNowPacket& raw_pkt = _rx_packets.read_head();
            Packet pkt;
            const bool decoded = Packet::from_payload(raw_pkt.src_mac, raw_pkt.payload, raw_pkt.payload_len, pkt, raw_pkt.des_mac);
            _rx_packets.pop();

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

void TransceiverBase::esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (!s_instance || s_instance->_tx_result_queue == nullptr) return;
    const bool success = status == ESP_NOW_SEND_SUCCESS;
    s_instance->on_hardware_tx_status(mac, success);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)xQueueSendFromISR(s_instance->_tx_result_queue, &success, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void TransceiverBase::esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_instance || !info->src_addr || !data || len < static_cast<int>(Packet::HEADER_SIZE)) return;
    if (len > ESP_NOW_MAX_DATA_LENGTH) return;

    CANId_t can_id = EspNowPacket::extract_can_id(data);
    if (s_instance->_stopping && can_id != CONTROL_ID) return;
    if (!s_instance->should_accept(can_id)) return;
    if (s_instance->_rx_packets.is_full()) return;

    auto& slot = s_instance->_rx_packets.write_head();
    std::memcpy(slot.src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(slot.des_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(slot.payload, data, static_cast<size_t>(len));
    slot.payload_len = static_cast<size_t>(len);
    s_instance->_rx_packets.push();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(s_instance->_recv_task_handle, NOTIFY_BIT_NEW_DATA,
                       eSetBits, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void TransceiverBase::stop(uint32_t timeout_ms) {
    if (_stopping && _radio_transmit_queue == nullptr) {
        return;
    }

    _stopping = true;
    _recv_callback = nullptr;

    // Cancel all linger timers
    for (auto& entry : _linger_timers) {
        if (entry.second != nullptr) {
            esp_timer_stop(entry.second);
        }
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (!_send_task_done || !_recv_task_done) {
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

    // Delete linger timers
    for (auto& entry : _linger_timers) {
        if (entry.second != nullptr) {
            esp_timer_delete(entry.second);
            entry.second = nullptr;
        }
    }

    if (s_instance == this) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        s_instance = nullptr;
    }

    // Delete queues
    if (_radio_transmit_queue) {
        vQueueDelete(_radio_transmit_queue);
        _radio_transmit_queue = nullptr;
    }
    if (_tx_result_queue) {
        vQueueDelete(_tx_result_queue);
        _tx_result_queue = nullptr;
    }
}

bool TransceiverBase::should_accept(CANId_t can_id) const {
    if (can_id == CONTROL_ID) return true;
    if (!_filtering_enabled) return true;
    return std::find(_rx_can_ids.begin(), _rx_can_ids.end(), can_id) != _rx_can_ids.end();
}
} // namespace wcan
