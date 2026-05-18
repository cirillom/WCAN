#include "TransceiverBase.hpp"

#include <cstring>
#include <algorithm>
#include <new>

#ifdef MEASURE_INSTR
#include "esp_timer.h"
extern volatile uint64_t g_airtime_total_us;
extern volatile uint64_t g_packets_sent_total;
#endif

namespace wcan {

static const char* TAG = "WCAN_BASE";

TransceiverBase* TransceiverBase::s_instance = nullptr;

TransceiverBase::~TransceiverBase() {
    if (_send_queue) vQueueDelete(_send_queue);
    if (_recv_queue) vQueueDelete(_recv_queue);
    for (auto &p : _can_data_queues) if (p.second) vQueueDelete(p.second);
    if (_tx_result_queue) vQueueDelete(_tx_result_queue);
    
    if (s_instance == this) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        s_instance = nullptr;
    }
}

TransceiverBase::TransceiverBase(std::vector<uint32_t> rx_can_ids, std::vector<uint32_t> tx_can_ids, uint32_t linger_ms, bool filtering_enabled)
    : _rx_can_ids(std::move(rx_can_ids)), 
      _tx_can_ids(std::move(tx_can_ids)), 
      _linger_ms(linger_ms),
      _filtering_enabled(filtering_enabled) {}

bool TransceiverBase::init() {
    if (s_instance != nullptr) return false;

    ESP_ERROR_CHECK(esp_read_mac(_mac_addr.data(), MAC_TYPE));

    _send_queue = xQueueCreate(QUEUE_SIZE, sizeof(Packet*));
    _recv_queue = xQueueCreate(QUEUE_SIZE, sizeof(EspNowPacket*));
    if (!_send_queue || !_recv_queue) return false;

    const size_t tx_count = _tx_can_ids.size();
    if (tx_count > 0) {
        for (size_t i = 0; i < tx_count; i++) {
            QueueHandle_t q = xQueueCreate(QUEUE_SIZE, sizeof(DataPoint_t));
            if (!q) return false;
            _can_data_queues.emplace(_tx_can_ids[i], q);
            _batch_task_handles.emplace(_tx_can_ids[i], (TaskHandle_t)nullptr);
            _pending_ack_seq_ids.emplace(_tx_can_ids[i], NO_PENDING_ACK_SEQUENCE_ID);
        }
    }

    _tx_result_queue = xQueueCreate(QUEUE_SIZE, sizeof(bool));
    if (!_tx_result_queue) return false;

    if (!setup_esp_now()) return false;
    start_tasks();
    return true;
}

bool TransceiverBase::send_data(CANId_t can_id, DataPoint_t data) {
    auto it = _can_data_queues.find(can_id);
    if (it == _can_data_queues.end()) return false;
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
    xTaskCreate(send_task_wrapper, "wcan_send", 4096, this, SEND_PROCESSING_TASK_PRIORITY, nullptr);
    xTaskCreate(recv_task_wrapper, "wcan_recv", 4096, this, RECV_PROCESSING_TASK_PRIORITY, nullptr);

    for (size_t i = 0; i < _tx_can_ids.size(); i++) {
        auto* ctx = new std::pair<TransceiverBase*, CANId_t>(this, _tx_can_ids[i]);
        char name[32]; std::snprintf(name, sizeof(name), "wcan_batch_%lu", (unsigned long)_tx_can_ids[i]);
        xTaskCreate(batch_task_wrapper, name, 4096, ctx, BATCH_PROCESSING_TASK_PRIORITY, &(_batch_task_handles[_tx_can_ids[i]]));
    }
}

void TransceiverBase::send_processing_task() {
    while (true) {
        Packet* raw_pkt_ptr = nullptr;
        if (xQueueReceive(_send_queue, &raw_pkt_ptr, portMAX_DELAY) == pdTRUE) {
            std::unique_ptr<Packet> pkt(raw_pkt_ptr);
            auto encoded = pkt->encode();
            if (!encoded) continue;

            // Determine destination using the virtual hook
            const uint8_t* dest_mac = prepare_send_mac(*pkt);
            
            for (int attempt = 0; attempt < RADIO_MAX_RETRIES; attempt++) {
                xQueueReset(_tx_result_queue); 

            #ifdef MEASURE_INSTR
                const int64_t send_start_us = esp_timer_get_time();
            #endif

                esp_err_t err = esp_now_send(dest_mac, encoded->data(), encoded->size());
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_now_send fail: %s", esp_err_to_name(err));
                    continue;
                }

                bool success = false;
                if (xQueueReceive(_tx_result_queue, &success, pdMS_TO_TICKS(RADIO_TIMEOUT_MS)) == pdTRUE) {
            #ifdef MEASURE_INSTR
                    const int64_t send_end_us = esp_timer_get_time();
                    g_packets_sent_total++;
                    g_airtime_total_us += static_cast<uint64_t>(send_end_us - send_start_us);
            #endif
                    if (success) {
                        break; 
                    }else {
                        std::printf("Radio send to %02x:%02x:%02x:%02x:%02x:%02x failed on attempt %d\n",
                            dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5], attempt + 1);
                    }
                }
            }
        }
    }
}

void TransceiverBase::recv_processing_task() {
    while (true) {
        EspNowPacket* raw_pkt_ptr = nullptr;
        if (xQueueReceive(_recv_queue, &raw_pkt_ptr, portMAX_DELAY) == pdTRUE) {
            std::unique_ptr<EspNowPacket> raw_pkt(raw_pkt_ptr);
            auto pkt_opt = Packet::from_payload(raw_pkt->src_mac, raw_pkt->payload, raw_pkt->payload_len, raw_pkt->des_mac);

            if (!pkt_opt) continue;

            const Packet& pkt = *pkt_opt;

            if (_deduplicator.check_and_update(pkt)) continue;

            if (pkt.get_can_id() == CONTROL_ID) {
                on_control_packet(pkt);
            } else {
                on_data_packet(pkt); 
                if (_recv_callback) _recv_callback(pkt);
            }
        }
    }
}

void TransceiverBase::batch_processing_task(CANId_t can_id) {
    auto it = _can_data_queues.find(can_id);
    if (it == _can_data_queues.end()) return;
    QueueHandle_t q = it->second;

    while (true) {
        DataPoint_t data;
        if (xQueueReceive(q, &data, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        Packet pkt(_mac_addr, can_id);
        pkt.add_data_point(data);

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(_linger_ms);
        while (true) {
            if (pkt.get_data().size() >= Packet::MAX_DATA_POINTS) break;

            const TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(deadline - now) <= 0) break;

            if (xQueueReceive(q, &data, deadline - now) == pdTRUE) {
                if (!pkt.add_data_point(data)) {
                    ESP_LOGE(TAG, "Failed to add data point to packet");
                    break;
                }
            } else {
                break;
            }
        }
        // Hand the constructed batch to the Strategy to handle retries/dispatch
        dispatch_packet(pkt, can_id);
    }
}

void TransceiverBase::esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (!s_instance) return;
    const bool success = status == ESP_NOW_SEND_SUCCESS;
    s_instance->on_hardware_tx_status(mac, success);
    xQueueSend(s_instance->_tx_result_queue, &success, 0);
}

void TransceiverBase::esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_instance || !info->src_addr || !data || len < Packet::HEADER_SIZE) return;

    CANId_t can_id = EspNowPacket::extract_can_id(data);

    if (!s_instance->should_accept(can_id)) return;

    auto* raw_pkt = new (std::nothrow) EspNowPacket;
    if (!raw_pkt) return;

    std::memcpy(raw_pkt->src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->des_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt->payload, data, (size_t)len);
    raw_pkt->payload_len = (size_t)len;

    if (xQueueSend(s_instance->_recv_queue, &raw_pkt, 0) != pdTRUE) {
        delete raw_pkt;
    }
}

bool TransceiverBase::should_accept(CANId_t can_id) const {
    if (can_id == CONTROL_ID) return true;
    if (!_filtering_enabled) return true;
    return std::find(_rx_can_ids.begin(), _rx_can_ids.end(), can_id) != _rx_can_ids.end();
}
} // namespace wcan