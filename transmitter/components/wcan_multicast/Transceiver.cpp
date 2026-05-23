#include "Transceiver.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wcan {

static const char* TAG = "WCAN_MCAST";

Transceiver::~Transceiver() {
    stop(100);
}

void Transceiver::stop(uint32_t timeout_ms) {
    _stopping = true;
    stop_management_task(timeout_ms);
    TransceiverBase::stop(timeout_ms);
    cleanup_multicast_resources();
}

bool Transceiver::init() {
    if (_state_mutex == nullptr) {
        _state_mutex = xSemaphoreCreateMutex();
        if (_state_mutex == nullptr) {
            ESP_LOGE(TAG, "Failed to create multicast state mutex");
            return false;
        }
    }

    if (_tx_status_events == nullptr) {
        _tx_status_events = xQueueCreate(TX_STATUS_QUEUE_SIZE, sizeof(TxStatusEvent));
        if (_tx_status_events == nullptr) {
            ESP_LOGE(TAG, "Failed to create multicast TX status queue");
            cleanup_multicast_resources();
            return false;
        }
    }

    if (!TransceiverBase::init()) {
        cleanup_multicast_resources();
        return false;
    }

    // Create CONTROL_ID ring for subscriptions/control packets
    auto& ctrl_ring = _tx_rings[CONTROL_ID];
    for (auto& slot : ctrl_ring.data) {
        slot.init_for_ring(_mac_addr, CONTROL_ID);
        slot.clear();
    }

    _management_task_done = false;
    if (xTaskCreate(management_task_wrapper,
                    "wcan_mcast",
                    MANAGEMENT_TASK_STACK_SIZE,
                    this,
                    MANAGEMENT_TASK_PRIORITY,
                    &_management_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create multicast management task");
        _management_task_done = true;
        TransceiverBase::stop(100);
        cleanup_multicast_resources();
        return false;
    }

    return true;
}

const uint8_t* Transceiver::prepare_send_mac(const Packet& packet) {
    if (packet.get_can_id() == CONTROL_ID) {
        lock_state();
        const bool has_unicast_destination = consume_pending_control_destination(packet.get_sequence_id(), _prepared_control_destination);
        if (has_unicast_destination) {
            const bool peer_ready = ensure_peer_locked(_prepared_control_destination.bytes.data());
            unlock_state();
            return peer_ready ? _prepared_control_destination.bytes.data() : Packet::BROADCAST_MAC.data();
        }

        const bool broadcast_ready = ensure_peer_locked(Packet::BROADCAST_MAC.data());
        unlock_state();
        return broadcast_ready ? Packet::BROADCAST_MAC.data() : nullptr;
    }

    lock_state();
    size_t synced_count = 0;
    const bool synced = sync_peers_for_can_id_locked(packet.get_can_id(), &synced_count);
    if (synced && synced_count > 0) {
        unlock_state();
        return nullptr;
    }

    const bool broadcast_ready = ensure_peer_locked(Packet::BROADCAST_MAC.data());
    unlock_state();
    return broadcast_ready ? Packet::BROADCAST_MAC.data() : nullptr;
}

void Transceiver::dispatch_batch(CANId_t can_id) {
    if (can_id == CONTROL_ID) {
        auto it = _tx_rings.find(CONTROL_ID);
        if (it == _tx_rings.end()) return;
        auto& ring = it->second;
        Packet* pkt_ptr = &ring.read_head();
        if (xQueueSend(_radio_transmit_queue, &pkt_ptr, 0) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send subscription control packet");
            forget_pending_control_destination(pkt_ptr->get_sequence_id());
            ring.read_head().clear();
            ring.pop();
        }
        return;
    }

    auto it = _tx_rings.find(can_id);
    if (it == _tx_rings.end()) return;
    auto& ring = it->second;

    Packet* pkt_ptr = &ring.read_head();
    if (xQueueSend(_radio_transmit_queue, &pkt_ptr, is_stopping() ? 0 : portMAX_DELAY) != pdTRUE) {
        on_radio_send(can_id, false);
    }
}

void Transceiver::on_control_packet(const Packet& packet) {
    const auto& source = packet.get_source_mac_addr();
    if (source == _mac_addr) return;

    lock_state();
    const bool accepted = _subscriptions.update(source.data(), packet.get_data(), _tx_can_ids, xTaskGetTickCount());
    unlock_state();

    if (accepted) {
        ESP_LOGD(TAG, "Updated subscriber %02x:%02x:%02x:%02x:%02x:%02x",
                 source[0], source[1], source[2], source[3], source[4], source[5]);
    }
}

void Transceiver::on_data_packet(const Packet& packet) {
    if (!packet.is_received_via_broadcast() || !has_rx_interest()) return;

    const auto& dest_mac = packet.get_source_mac_addr();
    if (!send_subscription(dest_mac.data())) {
        ESP_LOGW(TAG, "Failed to queue unicast subscription response");
    }
}

void Transceiver::on_hardware_tx_status(const uint8_t* mac_addr, bool success) {
    if (_tx_status_events == nullptr) return;

    TxStatusEvent event = {};
    event.success = success;
    event.has_mac = mac_addr != nullptr;
    if (event.has_mac) {
        copy_mac(event.mac, mac_addr);
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    (void)xQueueSendFromISR(_tx_status_events, &event, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
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

    auto it = _tx_rings.find(can_id);
    if (it == _tx_rings.end()) return;
    auto& ring = it->second;

    if (!success) {
        if (!ring.is_empty()) {
            const auto& data = ring.read_head().get_data();
            if (!data.empty()) {
                stats().record_sensor_send_failure(can_id, data.front(), data.back());
            }
            ESP_LOGW(TAG, "Multicast radio send reported failure for CAN ID 0x%08lx",
                     static_cast<unsigned long>(can_id));
        }
    }
    if (!ring.is_empty()) {
        ring.read_head().clear();
        ring.pop();
    }
}

bool Transceiver::add_peer(const uint8_t* mac_addr) {
    lock_state();
    const bool ok = ensure_peer_locked(mac_addr);
    unlock_state();
    return ok;
}

bool Transceiver::has_rx_interest() const {
    return !_filtering_enabled || !_rx_can_ids.empty();
}

bool Transceiver::fill_subscription_packet(Packet& packet) const {
    if (!_filtering_enabled) {
        return true;
    }

    if (_rx_can_ids.empty()) {
        return false;
    }

    for (CANId_t can_id : _rx_can_ids) {
        if (!packet.add_data_point(can_id)) {
            ESP_LOGW(TAG, "Subscription packet full; truncating advertised RX CAN IDs");
            break;
        }
    }
    return true;
}

bool Transceiver::send_subscription(const uint8_t* unicast_mac) {
    if (is_stopping()) return false;

    auto it = _tx_rings.find(CONTROL_ID);
    if (it == _tx_rings.end()) return false;
    auto& ring = it->second;

    if (ring.is_full()) {
        ESP_LOGW(TAG, "Failed to queue subscription: CONTROL ring full");
        return false;
    }

    auto& pkt = ring.write_head();
    pkt.clear();

    if (!fill_subscription_packet(pkt)) {
        return true;
    }

    pkt.assign_new_sequence_id();
    const uint32_t seq_id = pkt.get_sequence_id();

    if (unicast_mac != nullptr) {
        remember_pending_control_destination(seq_id, unicast_mac);
    }

    ring.push();

    dispatch_batch(CONTROL_ID);
    return true;
}

void Transceiver::remember_pending_control_destination(uint32_t sequence_id, const uint8_t* mac_addr) {
    lock_state();

    PendingControlDestination* slot = nullptr;
    for (auto& entry : _pending_control_destinations) {
        if (!entry.used || entry.sequence_id == sequence_id) {
            slot = &entry;
            break;
        }
    }

    if (slot == nullptr) {
        slot = &_pending_control_destinations[0];
        for (auto& entry : _pending_control_destinations) {
            if (static_cast<int32_t>(entry.created_at - slot->created_at) < 0) {
                slot = &entry;
            }
        }
    }

    slot->used = true;
    slot->sequence_id = sequence_id;
    slot->created_at = xTaskGetTickCount();
    copy_mac(slot->mac, mac_addr);

    unlock_state();
}

bool Transceiver::consume_pending_control_destination(uint32_t sequence_id, MacAddress& out) {
    for (auto& entry : _pending_control_destinations) {
        if (entry.used && entry.sequence_id == sequence_id) {
            out = entry.mac;
            entry.used = false;
            return true;
        }
    }
    return false;
}

void Transceiver::forget_pending_control_destination(uint32_t sequence_id) {
    lock_state();
    for (auto& entry : _pending_control_destinations) {
        if (entry.used && entry.sequence_id == sequence_id) {
            entry.used = false;
            break;
        }
    }
    unlock_state();
}

bool Transceiver::ensure_peer_locked(const uint8_t* mac_addr) {
    if (mac_addr == nullptr) return false;

    esp_now_peer_info_t peer = {};
    std::memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(peer.peer_addr)) {
        const esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(TAG, "Failed to add peer %02x:%02x:%02x:%02x:%02x:%02x: %s",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
                     esp_err_to_name(err));
            return false;
        }
    }

    return remember_known_peer_locked(mac_addr);
}

bool Transceiver::remove_peer_locked(const uint8_t* mac_addr) {
    if (mac_addr == nullptr) return true;

    if (esp_now_is_peer_exist(mac_addr)) {
        const esp_err_t err = esp_now_del_peer(mac_addr);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to remove peer %02x:%02x:%02x:%02x:%02x:%02x: %s",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
                     esp_err_to_name(err));
            return false;
        }
    }

    forget_known_peer_locked(mac_addr);
    return true;
}

bool Transceiver::sync_peers_for_can_id_locked(CANId_t can_id, size_t* synced_count) {
    std::array<MacAddress, MAX_UNICAST_PEERS> targets{};
    const size_t target_count = _subscriptions.collect_alive(can_id, xTaskGetTickCount(), targets);
    if (synced_count != nullptr) *synced_count = target_count;
    if (target_count == 0) return true;

    if (_broadcast_peer_known && !remove_peer_locked(Packet::BROADCAST_MAC.data())) {
        return false;
    }

    for (size_t i = 0; i < _known_unicast_peer_used.size(); ++i) {
        if (_known_unicast_peer_used[i] && !known_peer_is_target_locked(_known_unicast_peers[i], targets, target_count)) {
            if (!remove_peer_locked(_known_unicast_peers[i].bytes.data())) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < target_count; ++i) {
        if (!ensure_peer_locked(targets[i].bytes.data())) {
            return false;
        }
    }

    return true;
}

bool Transceiver::remember_known_peer_locked(const uint8_t* mac_addr) {
    if (is_broadcast_mac(mac_addr)) {
        _broadcast_peer_known = true;
        return true;
    }

    for (size_t i = 0; i < _known_unicast_peer_used.size(); ++i) {
        if (_known_unicast_peer_used[i] && mac_equals(_known_unicast_peers[i], mac_addr)) {
            return true;
        }
    }

    for (size_t i = 0; i < _known_unicast_peer_used.size(); ++i) {
        if (!_known_unicast_peer_used[i]) {
            _known_unicast_peer_used[i] = true;
            copy_mac(_known_unicast_peers[i], mac_addr);
            return true;
        }
    }

    ESP_LOGE(TAG, "Known peer cache full");
    return false;
}

void Transceiver::forget_known_peer_locked(const uint8_t* mac_addr) {
    if (is_broadcast_mac(mac_addr)) {
        _broadcast_peer_known = false;
        return;
    }

    for (size_t i = 0; i < _known_unicast_peer_used.size(); ++i) {
        if (_known_unicast_peer_used[i] && mac_equals(_known_unicast_peers[i], mac_addr)) {
            _known_unicast_peer_used[i] = false;
            return;
        }
    }
}

bool Transceiver::known_peer_is_target_locked(const MacAddress& peer,
                                              const std::array<MacAddress, MAX_UNICAST_PEERS>& targets,
                                              size_t target_count) const {
    (void)this;
    for (size_t i = 0; i < target_count; ++i) {
        if (mac_equals(peer, targets[i])) return true;
    }
    return false;
}

void Transceiver::drain_tx_status_events() {
    if (_tx_status_events == nullptr) return;

    TxStatusEvent event = {};
    while (xQueueReceive(_tx_status_events, &event, 0) == pdTRUE) {
        handle_tx_status_event(event);
    }
}

void Transceiver::handle_tx_status_event(const TxStatusEvent& event) {
    if (!event.has_mac || is_broadcast_mac(event.mac.bytes.data())) return;

    lock_state();
    if (event.success) {
        (void)_subscriptions.refresh(event.mac.bytes.data(), xTaskGetTickCount());
    }
    unlock_state();

    if (!event.success) {
        ESP_LOGD(TAG, "Hardware TX failed to %02x:%02x:%02x:%02x:%02x:%02x",
                 event.mac.bytes[0], event.mac.bytes[1], event.mac.bytes[2],
                 event.mac.bytes[3], event.mac.bytes[4], event.mac.bytes[5]);
    }
}

void Transceiver::management_task() {
    TickType_t last_subscription = xTaskGetTickCount() - pdMS_TO_TICKS(SUBSCRIPTION_INTERVAL_MS);

    while (!is_stopping()) {
        drain_tx_status_events();

        const TickType_t now = xTaskGetTickCount();
        if (static_cast<TickType_t>(now - last_subscription) >= pdMS_TO_TICKS(SUBSCRIPTION_INTERVAL_MS)) {
            (void)send_subscription(nullptr);
            last_subscription = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    drain_tx_status_events();
    _management_task_done = true;
    vTaskDelete(nullptr);
}

void Transceiver::stop_management_task(uint32_t timeout_ms) {
    if (_management_task_done || _management_task_handle == nullptr) {
        _management_task_handle = nullptr;
        _management_task_done = true;
        return;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (!_management_task_done) {
        if (timeout_ms == 0 || static_cast<int32_t>(xTaskGetTickCount() - start) >= static_cast<int32_t>(timeout_ticks)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!_management_task_done && _management_task_handle != nullptr) {
        vTaskDelete(_management_task_handle);
        _management_task_done = true;
    }
    _management_task_handle = nullptr;
}

void Transceiver::cleanup_multicast_resources() {
    stop_management_task(0);

    if (_tx_status_events != nullptr) {
        vQueueDelete(_tx_status_events);
        _tx_status_events = nullptr;
    }

    if (_state_mutex != nullptr) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
    }
}

void Transceiver::lock_state() {
    if (_state_mutex != nullptr) {
        xSemaphoreTake(_state_mutex, portMAX_DELAY);
    }
}

void Transceiver::unlock_state() {
    if (_state_mutex != nullptr) {
        xSemaphoreGive(_state_mutex);
    }
}

bool Transceiver::is_broadcast_mac(const uint8_t* mac_addr) {
    return mac_addr != nullptr && std::memcmp(mac_addr, Packet::BROADCAST_MAC.data(), ESP_NOW_ETH_ALEN) == 0;
}

bool Transceiver::mac_equals(const MacAddress& lhs, const uint8_t* rhs) {
    return rhs != nullptr && std::memcmp(lhs.bytes.data(), rhs, ESP_NOW_ETH_ALEN) == 0;
}

bool Transceiver::mac_equals(const MacAddress& lhs, const MacAddress& rhs) {
    return lhs.bytes == rhs.bytes;
}

void Transceiver::copy_mac(MacAddress& dest, const uint8_t* src) {
    if (src != nullptr) {
        std::memcpy(dest.bytes.data(), src, ESP_NOW_ETH_ALEN);
    }
}

bool Transceiver::SubscriptionTable::update(const uint8_t* mac_addr,
                                             std::span<const DataPoint_t> requested_can_ids,
                                             const std::vector<CANId_t>& tx_can_ids,
                                             TickType_t now) {
    if (mac_addr == nullptr || tx_can_ids.empty()) return false;

    const bool accepts_all = requested_can_ids.empty();
    std::array<CANId_t, Packet::MAX_DATA_POINTS> filtered_can_ids{};
    size_t filtered_count = 0;

    if (!accepts_all) {
        for (CANId_t can_id : requested_can_ids) {
            if (!contains_can_id(tx_can_ids, can_id)) continue;
            if (contains_can_id(std::span<const CANId_t>(filtered_can_ids.data(), filtered_count), can_id)) continue;
            filtered_can_ids[filtered_count++] = can_id;
            if (filtered_count >= filtered_can_ids.size()) break;
        }

        if (filtered_count == 0) {
            (void)remove(mac_addr);
            return false;
        }
    }

    Entry* entry = find(mac_addr);
    if (entry == nullptr) {
        entry = find_slot_for_update(now);
        if (entry == nullptr) return false;
    }

    entry->used = true;
    entry->accepts_all = accepts_all;
    entry->last_seen = now;
    Transceiver::copy_mac(entry->mac, mac_addr);
    entry->can_id_count = filtered_count;
    for (size_t i = 0; i < filtered_count; ++i) {
        entry->can_ids[i] = filtered_can_ids[i];
    }

    return true;
}

bool Transceiver::SubscriptionTable::refresh(const uint8_t* mac_addr, TickType_t now) {
    Entry* entry = find(mac_addr);
    if (entry == nullptr) return false;
    entry->last_seen = now;
    return true;
}

bool Transceiver::SubscriptionTable::remove(const uint8_t* mac_addr) {
    Entry* entry = find(mac_addr);
    if (entry == nullptr) return false;
    entry->used = false;
    return true;
}

size_t Transceiver::SubscriptionTable::collect_alive(CANId_t can_id,
                                                      TickType_t now,
                                                      std::array<MacAddress, ESP_NOW_MAX_TOTAL_PEER_NUM - 1>& out) const {
    size_t count = 0;
    for (const auto& entry : _entries) {
        if (!is_alive(entry, now) || !accepts_can_id(entry, can_id)) continue;
        out[count++] = entry.mac;
        if (count >= out.size()) break;
    }
    return count;
}

Transceiver::SubscriptionTable::Entry* Transceiver::SubscriptionTable::find(const uint8_t* mac_addr) {
    for (auto& entry : _entries) {
        if (entry.used && mac_equals(entry.mac, mac_addr)) return &entry;
    }
    return nullptr;
}

const Transceiver::SubscriptionTable::Entry* Transceiver::SubscriptionTable::find(const uint8_t* mac_addr) const {
    for (const auto& entry : _entries) {
        if (entry.used && mac_equals(entry.mac, mac_addr)) return &entry;
    }
    return nullptr;
}

Transceiver::SubscriptionTable::Entry* Transceiver::SubscriptionTable::find_slot_for_update(TickType_t now) {
    Entry* oldest = nullptr;
    for (auto& entry : _entries) {
        if (!entry.used || !is_alive(entry, now)) return &entry;
        if (oldest == nullptr || static_cast<int32_t>(entry.last_seen - oldest->last_seen) < 0) {
            oldest = &entry;
        }
    }
    return oldest;
}

bool Transceiver::SubscriptionTable::mac_equals(const MacAddress& lhs, const uint8_t* rhs) {
    return Transceiver::mac_equals(lhs, rhs);
}

bool Transceiver::SubscriptionTable::contains_can_id(const std::vector<CANId_t>& can_ids, CANId_t can_id) {
    return std::find(can_ids.begin(), can_ids.end(), can_id) != can_ids.end();
}

bool Transceiver::SubscriptionTable::contains_can_id(std::span<const CANId_t> can_ids, CANId_t can_id) {
    return std::find(can_ids.begin(), can_ids.end(), can_id) != can_ids.end();
}

bool Transceiver::SubscriptionTable::is_alive(const Entry& entry, TickType_t now) {
    return entry.used && static_cast<TickType_t>(now - entry.last_seen) <= pdMS_TO_TICKS(Transceiver::SUBSCRIBER_TTL_MS);
}

bool Transceiver::SubscriptionTable::accepts_can_id(const Entry& entry, CANId_t can_id) {
    if (entry.accepts_all) return true;
    return contains_can_id(std::span<const CANId_t>(entry.can_ids.data(), entry.can_id_count), can_id);
}

} // namespace wcan
