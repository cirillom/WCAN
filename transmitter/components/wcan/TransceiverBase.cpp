#include "TransceiverBase.hpp"

#include <cstring>
#include <algorithm>

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
    for (QueueHandle_t q : _can_data_queues) if (q) vQueueDelete(q);
    if (_radio_status_sem) vSemaphoreDelete(_radio_status_sem);
    for (SemaphoreHandle_t s : _ack_semaphores) if (s) vSemaphoreDelete(s);
    
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
    _recv_queue = xQueueCreate(QUEUE_SIZE, sizeof(EspNowPacket));
    if (!_send_queue || !_recv_queue) return false;

    const size_t tx_count = _tx_can_ids.size();
    if (tx_count > 0) {
        _can_data_queues.resize(tx_count);
        _ack_semaphores.resize(tx_count);
        for (size_t i = 0; i < tx_count; i++) {
            _can_data_queues[i] = xQueueCreate(QUEUE_SIZE, sizeof(DataPoint_t));
            _ack_semaphores[i] = xSemaphoreCreateBinary();
            if (!_can_data_queues[i] || !_ack_semaphores[i]) return false;
        }
    }

    _radio_status_sem = xSemaphoreCreateBinary();
    if (!_radio_status_sem) return false;

    if (!setup_esp_now()) return false;
    start_tasks();
    return true;
}

bool TransceiverBase::send_data(CANId_t can_id, DataPoint_t data) {
    size_t idx = get_can_queue_index(can_id);
    if (idx == SIZE_MAX) return false;
    return xQueueSend(_can_data_queues[idx], &data, 0) == pdTRUE;
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
        auto* ctx = new std::pair<TransceiverBase*, size_t>(this, i);
        char name[22]; std::snprintf(name, sizeof(name), "wcan_batch_%u", (unsigned)i);
        xTaskCreate(batch_task_wrapper, name, 4096, ctx, BATCH_PROCESSING_TASK_PRIORITY, nullptr);
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
            
#ifdef MEASURE_INSTR
            const int64_t send_start_us = esp_timer_get_time();
#endif
            esp_err_t err = esp_now_send(dest_mac, encoded->data(), encoded->size());
            if (err == ESP_OK) {
                // Wait for hardware callback
                xSemaphoreTake(_radio_status_sem, pdMS_TO_TICKS(RADIO_TIMEOUT_MS));
#ifdef MEASURE_INSTR
                const int64_t send_end_us = esp_timer_get_time();
                g_packets_sent_total++;
                if (send_end_us > send_start_us) {
                    g_airtime_total_us += static_cast<uint64_t>(send_end_us - send_start_us);
                }
#endif
            } else {
                ESP_LOGE(TAG, "esp_now_send fail: %s", esp_err_to_name(err));
            }
        }
    }
}

void TransceiverBase::recv_processing_task() {
    while (true) {
        EspNowPacket raw_pkt;
        if (xQueueReceive(_recv_queue, &raw_pkt, portMAX_DELAY) == pdTRUE) {
            auto pkt_opt = Packet::from_payload(raw_pkt.src_mac, raw_pkt.payload, raw_pkt.payload_len, raw_pkt.des_mac);
            
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

void TransceiverBase::batch_processing_task(size_t queue_index) {
    const uint32_t can_id = _tx_can_ids[queue_index];
    QueueHandle_t q = _can_data_queues[queue_index];

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
        dispatch_packet(pkt, queue_index);
    }
}

void TransceiverBase::esp_now_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (!s_instance) return;
    s_instance->on_hardware_tx_status(mac, status == ESP_NOW_SEND_SUCCESS);
    xSemaphoreGive(s_instance->_radio_status_sem);
}

void TransceiverBase::esp_now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_instance || !info->src_addr || !data || len < Packet::HEADER_SIZE) return;

    CANId_t can_id = EspNowPacket::extract_can_id(data);

    if (!s_instance->should_accept(can_id)) return;

    EspNowPacket raw_pkt;
    std::memcpy(raw_pkt.src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt.des_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    std::memcpy(raw_pkt.payload, data, (size_t)len);
    raw_pkt.payload_len = (size_t)len;

    xQueueSend(s_instance->_recv_queue, &raw_pkt, 0);
}

size_t TransceiverBase::get_can_queue_index(uint32_t id) const {
    for (size_t i = 0; i < _tx_can_ids.size(); i++) if (_tx_can_ids[i] == id) return i;
    return SIZE_MAX;
}

bool TransceiverBase::should_accept(CANId_t can_id) const {
    if (can_id == CONTROL_ID) return true;
    if (!_filtering_enabled) return true;
    return std::find(_rx_can_ids.begin(), _rx_can_ids.end(), can_id) != _rx_can_ids.end();
}
} // namespace wcan