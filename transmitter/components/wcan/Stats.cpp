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
#endif

namespace wcan {

#ifdef MEASURE_INSTR

class MeasuredStats final : public Stats {
public:
    void reset() override;
    int64_t now_us() const override { return esp_timer_get_time(); }
    void record_rx_packet(const Packet& packet) override;
    void record_airtime(uint32_t duration_us) override;
    void record_batch(CANId_t can_id, uint32_t points, int64_t ready_us, uint32_t dispatch_us) override;
    void finish_test() override;
    void print_sensor_end(uint32_t generated_count) const override;
    void print_rx_ranges() const override;
    void print_batch_stats() const override;
    void print_measures() const override;

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
        uint64_t dispatch_total_us = 0;
        uint32_t max_dispatch_us = 0;
    };

    uint32_t elapsed_ms() const;

    std::unordered_map<CANId_t, std::vector<CounterRange>> _rx_ranges;
    std::unordered_map<CANId_t, BatchStats> _batch_stats;
    uint32_t _heap_start_free = 0;
    uint32_t _heap_start_min_free = 0;
    uint32_t _heap_start_largest = 0;
    int64_t _start_us = 0;
    int64_t _end_us = 0;
    uint64_t _airtime_total_us = 0;
    uint64_t _packets_sent_total = 0;
};

void MeasuredStats::reset() {
    _rx_ranges.clear();
    _batch_stats.clear();
    _heap_start_free = static_cast<uint32_t>(esp_get_free_heap_size());
    _heap_start_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    _heap_start_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    _start_us = esp_timer_get_time();
    _end_us = 0;
    _airtime_total_us = 0;
    _packets_sent_total = 0;
}

uint32_t MeasuredStats::elapsed_ms() const {
    const int64_t end_us = _end_us > 0 ? _end_us : esp_timer_get_time();
    if (_start_us <= 0 || end_us <= _start_us) return 0;
    return static_cast<uint32_t>((end_us - _start_us) / 1000);
}

void MeasuredStats::finish_test() {
    _end_us = esp_timer_get_time();
}

void MeasuredStats::record_rx_packet(const Packet& packet) {
    const auto data = packet.get_data();
    if (data.empty()) return;

    auto& ranges = _rx_ranges[packet.get_can_id()];
    const uint32_t first = data.front();
    const uint32_t last = data.back();
    if (!ranges.empty() && first == ranges.back().second + 1) {
        ranges.back().second = last;
        return;
    }
    ranges.emplace_back(first, last);
}

void MeasuredStats::record_airtime(uint32_t duration_us) {
    _airtime_total_us += duration_us;
    _packets_sent_total++;
}

void MeasuredStats::record_batch(CANId_t can_id, uint32_t points, int64_t ready_us, uint32_t dispatch_us) {
    auto& stats = _batch_stats[can_id];
    stats.batch_count++;
    stats.points_total += points;
    stats.min_points = std::min(stats.min_points, points);
    stats.max_points = std::max(stats.max_points, points);
    stats.dispatch_total_us += dispatch_us;
    stats.max_dispatch_us = std::max(stats.max_dispatch_us, dispatch_us);

    if (stats.last_ready_us > 0 && ready_us > stats.last_ready_us) {
        const uint32_t interval_us = static_cast<uint32_t>(ready_us - stats.last_ready_us);
        stats.interval_total_us += interval_us;
        stats.interval_count++;
        stats.min_interval_us = std::min(stats.min_interval_us, interval_us);
        stats.max_interval_us = std::max(stats.max_interval_us, interval_us);
    }
    stats.last_ready_us = ready_us;
}

void MeasuredStats::print_sensor_end(uint32_t generated_count) const {
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
}

void MeasuredStats::print_rx_ranges() const {
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
}

void MeasuredStats::print_batch_stats() const {
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
        const double avg_dispatch_ms = static_cast<double>(stats.dispatch_total_us) / static_cast<double>(stats.batch_count) / 1000.0;
        const uint32_t min_points = stats.min_points == UINT32_MAX ? 0 : stats.min_points;
        const double min_interval_ms = stats.min_interval_us == UINT32_MAX ? 0.0 : static_cast<double>(stats.min_interval_us) / 1000.0;

        std::printf(
            "WCAN_BATCH id=0x%lx count=%lu avg_hz=%.2f avg_points=%.2f min_points=%lu max_points=%lu avg_interval_ms=%.2f min_interval_ms=%.2f max_interval_ms=%.2f avg_dispatch_ms=%.2f max_dispatch_ms=%.2f\n",
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
            static_cast<double>(stats.max_dispatch_us) / 1000.0);
    }
    std::fflush(stdout);
}

void MeasuredStats::print_measures() const {
    const uint32_t heap_end_free = static_cast<uint32_t>(esp_get_free_heap_size());
    const uint32_t heap_end_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    const uint32_t heap_end_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    std::printf(
        "WCAN_MEASURES elapsed_ms=%lu heap_start_free=%lu heap_end_free=%lu heap_start_min_free=%lu heap_end_min_free=%lu heap_start_largest=%lu heap_end_largest=%lu airtime_us_total=%llu packets_sent_total=%llu\n",
        static_cast<unsigned long>(elapsed_ms()),
        static_cast<unsigned long>(_heap_start_free),
        static_cast<unsigned long>(heap_end_free),
        static_cast<unsigned long>(_heap_start_min_free),
        static_cast<unsigned long>(heap_end_min_free),
        static_cast<unsigned long>(_heap_start_largest),
        static_cast<unsigned long>(heap_end_largest),
        static_cast<unsigned long long>(_airtime_total_us),
        static_cast<unsigned long long>(_packets_sent_total));
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
