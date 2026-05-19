#include "Stats.hpp"

#include <memory>

#ifdef MEASURE_INSTR
#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace wcan {

#ifdef MEASURE_INSTR

class MeasuredStats final : public Stats {
public:
    void reset() override;
    int64_t now_us() const override { return esp_timer_get_time(); }
    void record_rx_packet(const Packet& packet) override;
    void record_airtime(uint32_t duration_us) override;
    void record_batch(const Packet& packet, int64_t ready_us) override;
    void record_batch_dispatch(const Packet& packet, int64_t dispatch_start_us, uint32_t dispatch_us) override;
    void record_sensor_send_failure(CANId_t can_id, uint32_t counter) override;
    void configure_tx_ids(const std::vector<CANId_t>& tx_ids) override;
    void finish_test() override;
    void print_sensor_end(uint32_t generated_count) const override;
    void print_rx_ranges() const override;
    void print_batch_stats() const override;
    void print_measures() const override;

    MeasuredStats() : _mutex(nullptr) { _mutex = xSemaphoreCreateMutex(); }
    ~MeasuredStats() {
        if (_mutex) vSemaphoreDelete(_mutex);
        for (auto s : _slot_mutexes) {
            if (s) vSemaphoreDelete(s);
        }
    }

private:
    using CounterRange = std::pair<uint32_t, uint32_t>;

    struct BatchStats {
        uint32_t batch_count = 0;
        uint64_t points_total = 0;
        uint32_t min_points = UINT32_MAX;
        uint32_t max_points = 0;
        int64_t last_ready_us = 0;
        uint64_t interval_total_us = 0;
        uint32_t interval_count = 0;
        uint32_t min_interval_us = UINT32_MAX;
        uint32_t max_interval_us = 0;
        uint32_t dispatched_count = 0;
        uint64_t queue_wait_total_us = 0;
        uint32_t max_queue_wait_us = 0;
        uint64_t dispatch_total_us = 0;
        uint32_t max_dispatch_us = 0;
    };

    uint32_t elapsed_ms() const;
    static uint64_t batch_key(CANId_t can_id, uint32_t sequence_id);

    std::unordered_map<CANId_t, std::vector<CounterRange>> _rx_ranges;
    std::unordered_map<CANId_t, BatchStats> _batch_stats;
    std::unordered_map<CANId_t, std::vector<CounterRange>> _sensor_send_failure_ranges;
    std::unordered_map<uint64_t, int64_t> _batch_ready_us_by_key;
    uint32_t _heap_start_free = 0;
    uint32_t _heap_start_min_free = 0;
    uint32_t _heap_start_largest = 0;
    int64_t _start_us = 0;
    int64_t _end_us = 0;
    uint64_t _airtime_total_us = 0;
    uint64_t _packets_sent_total = 0;
    uint64_t _sensor_send_failures_total = 0;
    mutable SemaphoreHandle_t _mutex;
    // Fixed-slot structures (optional, configured from TransceiverBase)
    bool _slots_configured = false;
    std::unordered_map<CANId_t, size_t> _slot_index;
    std::vector<CANId_t> _slot_to_can_id;
    std::vector<std::vector<CounterRange>> _rx_ranges_slots;
    std::vector<BatchStats> _batch_stats_slots;
    std::vector<std::vector<CounterRange>> _sensor_send_failure_ranges_slots;
    std::vector<std::unordered_map<uint32_t, int64_t>> _batch_ready_by_slot;
    std::vector<SemaphoreHandle_t> _slot_mutexes;
};

void MeasuredStats::reset() {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_slots_configured) {
        for (auto &v : _rx_ranges_slots) v.clear();
        for (auto &s : _batch_stats_slots) s = BatchStats();
        for (auto &v : _sensor_send_failure_ranges_slots) v.clear();
        for (auto &m : _batch_ready_by_slot) m.clear();
    } else {
        _rx_ranges.clear();
        _batch_stats.clear();
        _sensor_send_failure_ranges.clear();
        _batch_ready_us_by_key.clear();
    }
    _heap_start_free = static_cast<uint32_t>(esp_get_free_heap_size());
    _heap_start_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    _heap_start_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    _start_us = esp_timer_get_time();
    _end_us = 0;
    _airtime_total_us = 0;
    _packets_sent_total = 0;
    _sensor_send_failures_total = 0;
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::configure_tx_ids(const std::vector<CANId_t>& tx_ids) {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _slot_index.clear();
    _slot_to_can_id.clear();
    _rx_ranges_slots.clear();
    _batch_stats_slots.clear();
    _sensor_send_failure_ranges_slots.clear();
    _batch_ready_by_slot.clear();
    for (auto s : _slot_mutexes) { if (s) vSemaphoreDelete(s); }
    _slot_mutexes.clear();

    const size_t n = tx_ids.size();
    if (n == 0) {
        _slots_configured = false;
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }

    _slot_to_can_id.reserve(n);
    _rx_ranges_slots.resize(n);
    _batch_stats_slots.resize(n);
    _sensor_send_failure_ranges_slots.resize(n);
    _batch_ready_by_slot.resize(n);
    _slot_mutexes.resize(n);

    for (size_t i = 0; i < n; ++i) {
        const CANId_t id = tx_ids[i];
        _slot_index[id] = i;
        _slot_to_can_id.push_back(id);
        _rx_ranges_slots[i] = std::vector<CounterRange>();
        _batch_stats_slots[i] = BatchStats();
        _sensor_send_failure_ranges_slots[i] = std::vector<CounterRange>();
        _batch_ready_by_slot[i] = std::unordered_map<uint32_t, int64_t>();
        _slot_mutexes[i] = xSemaphoreCreateMutex();
    }
    _slots_configured = true;
    if (_mutex) xSemaphoreGive(_mutex);
}

uint64_t MeasuredStats::batch_key(CANId_t can_id, uint32_t sequence_id) {
    return (static_cast<uint64_t>(can_id) << 32) | static_cast<uint64_t>(sequence_id);
}

uint32_t MeasuredStats::elapsed_ms() const {
    const int64_t end_us = _end_us > 0 ? _end_us : esp_timer_get_time();
    if (_start_us <= 0 || end_us <= _start_us) return 0;
    return static_cast<uint32_t>((end_us - _start_us) / 1000);
}

void MeasuredStats::finish_test() {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _end_us = esp_timer_get_time();
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::record_rx_packet(const Packet& packet) {
    const auto data = packet.get_data();
    if (data.empty()) return;
    const CANId_t can_id = packet.get_can_id();
    const uint32_t first = data.front();
    const uint32_t last = data.back();

    if (_slots_configured) {
        auto it = _slot_index.find(can_id);
        if (it != _slot_index.end()) {
            const size_t idx = it->second;
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            auto &ranges = _rx_ranges_slots[idx];
            if (!ranges.empty() && first == ranges.back().second + 1) {
                ranges.back().second = last;
                if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
                return;
            }
            ranges.emplace_back(first, last);
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
            return;
        }
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    auto& ranges = _rx_ranges[can_id];
    if (!ranges.empty() && first == ranges.back().second + 1) {
        ranges.back().second = last;
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }
    ranges.emplace_back(first, last);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::record_airtime(uint32_t duration_us) {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _airtime_total_us += duration_us;
    _packets_sent_total++;
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::record_sensor_send_failure(CANId_t can_id, uint32_t counter) {
    if (_slots_configured) {
        auto it = _slot_index.find(can_id);
        if (it != _slot_index.end()) {
            const size_t idx = it->second;
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            _sensor_send_failure_ranges_slots[idx].emplace_back(counter, counter);
            _sensor_send_failures_total++;
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
            return;
        }
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    _sensor_send_failures_total++;
    auto& ranges = _sensor_send_failure_ranges[can_id];
    if (!ranges.empty() && counter == ranges.back().second + 1) {
        ranges.back().second = counter;
        if (_mutex) xSemaphoreGive(_mutex);
        return;
    }
    ranges.emplace_back(counter, counter);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::record_batch(const Packet& packet, int64_t ready_us) {
    const CANId_t can_id = packet.get_can_id();
    const uint32_t points = static_cast<uint32_t>(packet.get_data().size());
    const uint32_t seq = packet.get_sequence_id();

    if (_slots_configured) {
        auto it = _slot_index.find(can_id);
        if (it != _slot_index.end()) {
            const size_t idx = it->second;
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            auto &stats = _batch_stats_slots[idx];
            stats.batch_count++;
            stats.points_total += points;
            stats.min_points = std::min(stats.min_points, points);
            stats.max_points = std::max(stats.max_points, points);
            _batch_ready_by_slot[idx][seq] = ready_us;

            if (stats.last_ready_us > 0 && ready_us > stats.last_ready_us) {
                const uint32_t interval_us = static_cast<uint32_t>(ready_us - stats.last_ready_us);
                stats.interval_total_us += interval_us;
                stats.interval_count++;
                stats.min_interval_us = std::min(stats.min_interval_us, interval_us);
                stats.max_interval_us = std::max(stats.max_interval_us, interval_us);
            }
            stats.last_ready_us = ready_us;
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
            return;
        }
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    auto& stats = _batch_stats[can_id];
    stats.batch_count++;
    stats.points_total += points;
    stats.min_points = std::min(stats.min_points, points);
    stats.max_points = std::max(stats.max_points, points);
    _batch_ready_us_by_key[batch_key(can_id, seq)] = ready_us;

    if (stats.last_ready_us > 0 && ready_us > stats.last_ready_us) {
        const uint32_t interval_us = static_cast<uint32_t>(ready_us - stats.last_ready_us);
        stats.interval_total_us += interval_us;
        stats.interval_count++;
        stats.min_interval_us = std::min(stats.min_interval_us, interval_us);
        stats.max_interval_us = std::max(stats.max_interval_us, interval_us);
    }
    stats.last_ready_us = ready_us;
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::record_batch_dispatch(const Packet& packet, int64_t dispatch_start_us, uint32_t dispatch_us) {
    const CANId_t can_id = packet.get_can_id();
    uint32_t queue_wait_us = 0;
    const uint32_t seq = packet.get_sequence_id();

    if (_slots_configured) {
        auto it = _slot_index.find(can_id);
        if (it != _slot_index.end()) {
            const size_t idx = it->second;
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            auto ready_it = _batch_ready_by_slot[idx].find(seq);
            if (ready_it != _batch_ready_by_slot[idx].end()) {
                queue_wait_us = static_cast<uint32_t>(std::max<int64_t>(0, dispatch_start_us - ready_it->second));
                _batch_ready_by_slot[idx].erase(ready_it);
            }
            auto& stats = _batch_stats_slots[idx];
            stats.dispatched_count++;
            stats.queue_wait_total_us += queue_wait_us;
            stats.max_queue_wait_us = std::max(stats.max_queue_wait_us, queue_wait_us);
            stats.dispatch_total_us += dispatch_us;
            stats.max_dispatch_us = std::max(stats.max_dispatch_us, dispatch_us);
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
            return;
        }
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    const uint64_t key = batch_key(can_id, seq);
    auto ready_it = _batch_ready_us_by_key.find(key);
    if (ready_it != _batch_ready_us_by_key.end()) {
        queue_wait_us = static_cast<uint32_t>(std::max<int64_t>(0, dispatch_start_us - ready_it->second));
        _batch_ready_us_by_key.erase(ready_it);
    }

    auto& stats = _batch_stats[can_id];
    stats.dispatched_count++;
    stats.queue_wait_total_us += queue_wait_us;
    stats.max_queue_wait_us = std::max(stats.max_queue_wait_us, queue_wait_us);
    stats.dispatch_total_us += dispatch_us;
    stats.max_dispatch_us = std::max(stats.max_dispatch_us, dispatch_us);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::print_sensor_end(uint32_t generated_count) const {
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    const uint32_t duration_ms = elapsed_ms();
    const double avg_hz = duration_ms == 0 ? 0.0 :
        (static_cast<double>(generated_count) * 1000.0) / static_cast<double>(duration_ms);

    if (generated_count == 0) {
        std::printf("WCAN_SENSOR_END generated=none avg_hz=0.00\n");
    } else {
        std::printf("WCAN_SENSOR_END generated=%lu avg_hz=%.2f\n",
                    static_cast<unsigned long>(generated_count - 1), avg_hz);
    }
    std::fflush(stdout);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::print_rx_ranges() const {
    if (_slots_configured) {
        for (size_t idx = 0; idx < _slot_to_can_id.size(); ++idx) {
            const CANId_t can_id = _slot_to_can_id[idx];
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            const auto& ranges = _rx_ranges_slots[idx];
            std::printf("WCAN_RX_RANGE id=0x%lx ranges=", static_cast<unsigned long>(can_id));
            if (ranges.empty()) {
                std::printf("[]");
            } else {
                for (size_t i = 0; i < ranges.size(); ++i) {
                    if (i > 0) std::printf(",");
                    std::printf("[%lu..%lu]",
                                static_cast<unsigned long>(ranges[i].first),
                                static_cast<unsigned long>(ranges[i].second));
                }
            }
            std::printf("\n");
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
        }
        std::fflush(stdout);
        return;
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    std::vector<CANId_t> ids;
    ids.reserve(_rx_ranges.size());
    for (const auto& entry : _rx_ranges) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());

    for (CANId_t can_id : ids) {
        const auto& ranges = _rx_ranges.at(can_id);
        std::printf("WCAN_RX_RANGE id=0x%lx ranges=", static_cast<unsigned long>(can_id));
        if (ranges.empty()) {
            std::printf("[]");
        } else {
            for (size_t i = 0; i < ranges.size(); ++i) {
                if (i > 0) std::printf(",");
                std::printf("[%lu..%lu]",
                            static_cast<unsigned long>(ranges[i].first),
                            static_cast<unsigned long>(ranges[i].second));
            }
        }
        std::printf("\n");
    }
    std::fflush(stdout);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::print_batch_stats() const {
    if (_slots_configured) {
        for (size_t idx = 0; idx < _slot_to_can_id.size(); ++idx) {
            const CANId_t can_id = _slot_to_can_id[idx];
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            const BatchStats& stats = _batch_stats_slots[idx];
            if (stats.batch_count == 0) {
                if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
                continue;
            }

            const double avg_points = static_cast<double>(stats.points_total) / static_cast<double>(stats.batch_count);
            const double avg_interval_ms = stats.interval_count == 0 ? 0.0 :
                static_cast<double>(stats.interval_total_us) / static_cast<double>(stats.interval_count) / 1000.0;
            const double avg_hz = stats.interval_total_us == 0 ? 0.0 :
                (static_cast<double>(stats.interval_count) * 1000000.0) / static_cast<double>(stats.interval_total_us);
            const double avg_dispatch_ms = stats.dispatched_count == 0 ? 0.0 :
                static_cast<double>(stats.dispatch_total_us) / static_cast<double>(stats.dispatched_count) / 1000.0;
            const double avg_queue_wait_ms = stats.dispatched_count == 0 ? 0.0 :
                static_cast<double>(stats.queue_wait_total_us) / static_cast<double>(stats.dispatched_count) / 1000.0;
            const uint32_t min_points = stats.min_points == UINT32_MAX ? 0 : stats.min_points;
            const double min_interval_ms = stats.min_interval_us == UINT32_MAX ? 0.0 : static_cast<double>(stats.min_interval_us) / 1000.0;

            std::printf(
                "WCAN_BATCH id=0x%lx count=%lu avg_hz=%.2f avg_points=%.2f min_points=%lu max_points=%lu avg_interval_ms=%.2f min_interval_ms=%.2f max_interval_ms=%.2f avg_dispatch_ms=%.2f max_dispatch_ms=%.2f dispatched_count=%lu avg_queue_wait_ms=%.2f max_queue_wait_ms=%.2f\n",
                static_cast<unsigned long>(can_id),
                static_cast<unsigned long>(stats.batch_count),
                avg_hz,
                avg_points,
                static_cast<unsigned long>(min_points),
                static_cast<unsigned long>(stats.max_points),
                avg_interval_ms,
                min_interval_ms,
                static_cast<double>(stats.max_interval_us) / 1000.0,
                avg_dispatch_ms,
                static_cast<double>(stats.max_dispatch_us) / 1000.0,
                static_cast<unsigned long>(stats.dispatched_count),
                avg_queue_wait_ms,
                static_cast<double>(stats.max_queue_wait_us) / 1000.0);
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
        }
        std::fflush(stdout);
        return;
    }

    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    std::vector<CANId_t> ids;
    ids.reserve(_batch_stats.size());
    for (const auto& entry : _batch_stats) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());

    for (CANId_t can_id : ids) {
        const BatchStats& stats = _batch_stats.at(can_id);
        if (stats.batch_count == 0) continue;

        const double avg_points = static_cast<double>(stats.points_total) / static_cast<double>(stats.batch_count);
        const double avg_interval_ms = stats.interval_count == 0 ? 0.0 :
            static_cast<double>(stats.interval_total_us) / static_cast<double>(stats.interval_count) / 1000.0;
        const double avg_hz = stats.interval_total_us == 0 ? 0.0 :
            (static_cast<double>(stats.interval_count) * 1000000.0) / static_cast<double>(stats.interval_total_us);
        const double avg_dispatch_ms = stats.dispatched_count == 0 ? 0.0 :
            static_cast<double>(stats.dispatch_total_us) / static_cast<double>(stats.dispatched_count) / 1000.0;
        const double avg_queue_wait_ms = stats.dispatched_count == 0 ? 0.0 :
            static_cast<double>(stats.queue_wait_total_us) / static_cast<double>(stats.dispatched_count) / 1000.0;
        const uint32_t min_points = stats.min_points == UINT32_MAX ? 0 : stats.min_points;
        const double min_interval_ms = stats.min_interval_us == UINT32_MAX ? 0.0 : static_cast<double>(stats.min_interval_us) / 1000.0;

        std::printf(
            "WCAN_BATCH id=0x%lx count=%lu avg_hz=%.2f avg_points=%.2f min_points=%lu max_points=%lu avg_interval_ms=%.2f min_interval_ms=%.2f max_interval_ms=%.2f avg_dispatch_ms=%.2f max_dispatch_ms=%.2f dispatched_count=%lu avg_queue_wait_ms=%.2f max_queue_wait_ms=%.2f\n",
            static_cast<unsigned long>(can_id),
            static_cast<unsigned long>(stats.batch_count),
            avg_hz,
            avg_points,
            static_cast<unsigned long>(min_points),
            static_cast<unsigned long>(stats.max_points),
            avg_interval_ms,
            min_interval_ms,
            static_cast<double>(stats.max_interval_us) / 1000.0,
            avg_dispatch_ms,
            static_cast<double>(stats.max_dispatch_us) / 1000.0,
            static_cast<unsigned long>(stats.dispatched_count),
            avg_queue_wait_ms,
            static_cast<double>(stats.max_queue_wait_us) / 1000.0);
    }
    std::fflush(stdout);
    if (_mutex) xSemaphoreGive(_mutex);
}

void MeasuredStats::print_measures() const {
    if (_slots_configured) {
        for (size_t idx = 0; idx < _slot_to_can_id.size(); ++idx) {
            const CANId_t can_id = _slot_to_can_id[idx];
            if (_slot_mutexes[idx]) xSemaphoreTake(_slot_mutexes[idx], portMAX_DELAY);
            const auto& ranges = _sensor_send_failure_ranges_slots[idx];
            std::printf("WCAN_S_FAIL_RANGE id=0x%lx ranges=", static_cast<unsigned long>(can_id));
            if (ranges.empty()) {
                std::printf("[]");
            } else {
                for (size_t i = 0; i < ranges.size(); ++i) {
                    if (i > 0) std::printf(",");
                    std::printf("[%lu..%lu]",
                                static_cast<unsigned long>(ranges[i].first),
                                static_cast<unsigned long>(ranges[i].second));
                }
            }
            std::printf("\n");
            if (_slot_mutexes[idx]) xSemaphoreGive(_slot_mutexes[idx]);
        }
    } else {
        if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
        std::vector<CANId_t> fail_ids;
        fail_ids.reserve(_sensor_send_failure_ranges.size());
        for (const auto& entry : _sensor_send_failure_ranges) fail_ids.push_back(entry.first);
        std::sort(fail_ids.begin(), fail_ids.end());
        for (CANId_t can_id : fail_ids) {
            const auto& ranges = _sensor_send_failure_ranges.at(can_id);
            std::printf("WCAN_S_FAIL_RANGE id=0x%lx ranges=", static_cast<unsigned long>(can_id));
            if (ranges.empty()) {
                std::printf("[]");
            } else {
                for (size_t i = 0; i < ranges.size(); ++i) {
                    if (i > 0) std::printf(",");
                    std::printf("[%lu..%lu]",
                                static_cast<unsigned long>(ranges[i].first),
                                static_cast<unsigned long>(ranges[i].second));
                }
            }
            std::printf("\n");
        }
        if (_mutex) xSemaphoreGive(_mutex);
    }

    // Totals: read under global mutex to get consistent snapshot
    if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
    const uint32_t heap_end_free = static_cast<uint32_t>(esp_get_free_heap_size());
    const uint32_t heap_end_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    const uint32_t heap_end_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const uint64_t airtime = _airtime_total_us;
    const uint64_t packets = _packets_sent_total;
    const uint64_t sfail = _sensor_send_failures_total;
    if (_mutex) xSemaphoreGive(_mutex);

    std::printf(
        "WCAN_MEASURES elapsed_ms=%lu heap_start_free=%lu heap_end_free=%lu heap_start_min_free=%lu heap_end_min_free=%lu heap_start_largest=%lu heap_end_largest=%lu airtime_us_total=%llu packets_sent_total=%llu sensor_send_failures_total=%llu\n",
        static_cast<unsigned long>(elapsed_ms()),
        static_cast<unsigned long>(_heap_start_free),
        static_cast<unsigned long>(heap_end_free),
        static_cast<unsigned long>(_heap_start_min_free),
        static_cast<unsigned long>(heap_end_min_free),
        static_cast<unsigned long>(_heap_start_largest),
        static_cast<unsigned long>(heap_end_largest),
        static_cast<unsigned long long>(airtime),
        static_cast<unsigned long long>(packets),
        static_cast<unsigned long long>(sfail));
    std::fflush(stdout);
}

#endif // MEASURE_INSTR

std::unique_ptr<Stats> make_stats() {
#ifdef MEASURE_INSTR
    return std::make_unique<MeasuredStats>();
#else
    return std::make_unique<Stats>();
#endif
}

} // namespace wcan
