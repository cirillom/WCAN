#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime_config.hpp"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wcan.hpp"

#ifdef MEASURE_INSTR
volatile uint64_t g_airtime_total_us = 0;
volatile uint64_t g_packets_sent_total = 0;
#else
static volatile uint64_t g_airtime_total_us = 0;
static volatile uint64_t g_packets_sent_total = 0;
#endif

namespace app_stats_detail {

using CounterRange = std::pair<uint32_t, uint32_t>;

static std::unordered_map<uint32_t, std::vector<CounterRange>> s_rx_ranges;
static uint32_t s_heap_start_free = 0;
static uint32_t s_heap_start_min_free = 0;
static uint32_t s_heap_start_largest = 0;
static int64_t s_stats_start_us = 0;

inline void Reset(void)
{
    s_rx_ranges.clear();
    s_heap_start_free = static_cast<uint32_t>(esp_get_free_heap_size());
    s_heap_start_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    s_heap_start_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_stats_start_us = esp_timer_get_time();
}

inline void RecordPacketStats(const wcan::Packet &recv_packet)
{
    const auto& data = recv_packet.get_data();
    if (data.empty()) {
        return;
    }

    auto& ranges = s_rx_ranges[recv_packet.get_can_id()];
    const uint32_t first = data.front();
    const uint32_t last = data.back();
    if (!ranges.empty() && first == ranges.back().second + 1) {
        ranges.back().second = last;
        return;
    }
    ranges.emplace_back(first, last);
}

inline void PrintRxRanges(void)
{
    std::vector<uint32_t> ids;
    ids.reserve(s_rx_ranges.size());
    for (const auto& entry : s_rx_ranges) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    for (uint32_t can_id : ids) {
        const auto& ranges = s_rx_ranges[can_id];
        std::printf("WCAN_RX_RANGE id=0x%lx ranges=", static_cast<unsigned long>(can_id));
        if (ranges.empty()) {
            std::printf("[]");
        } else {
            for (size_t i = 0; i < ranges.size(); ++i) {
                if (i > 0) {
                    std::printf(",");
                }
                std::printf("[%lu..%lu]",
                            static_cast<unsigned long>(ranges[i].first),
                            static_cast<unsigned long>(ranges[i].second));
            }
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

inline uint32_t ElapsedMsSinceStart(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_stats_start_us <= 0 || now <= s_stats_start_us) {
        return 0;
    }
    return static_cast<uint32_t>((now - s_stats_start_us) / 1000);
}

inline void PrintMeasures(void)
{
    const uint32_t heap_end_free = static_cast<uint32_t>(esp_get_free_heap_size());
    const uint32_t heap_end_min_free = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    const uint32_t heap_end_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    std::printf(
        "WCAN_MEASURES elapsed_ms=%lu heap_start_free=%lu heap_end_free=%lu heap_start_min_free=%lu heap_end_min_free=%lu heap_start_largest=%lu heap_end_largest=%lu airtime_us_total=%llu packets_sent_total=%llu\n",
        static_cast<unsigned long>(ElapsedMsSinceStart()),
        static_cast<unsigned long>(s_heap_start_free),
        static_cast<unsigned long>(heap_end_free),
        static_cast<unsigned long>(s_heap_start_min_free),
        static_cast<unsigned long>(heap_end_min_free),
        static_cast<unsigned long>(s_heap_start_largest),
        static_cast<unsigned long>(heap_end_largest),
        static_cast<unsigned long long>(g_airtime_total_us),
        static_cast<unsigned long long>(g_packets_sent_total));
    std::fflush(stdout);
}

} // namespace app_stats_detail

inline void start_app_stats(void)
{
    app_stats_detail::Reset();
}
