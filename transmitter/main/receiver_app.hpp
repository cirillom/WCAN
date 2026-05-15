#pragma once

#include <array>
#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef MEASURE_INSTR
#include "esp_timer.h"
#endif

#include "app_config.hpp"
#include "wcan.hpp"

namespace receiver_app_detail {

struct RxStats {
    bool valid = false;
    uint32_t can_id = 0;
    uint32_t last_counter = 0;
    uint32_t last_first = 0;
    uint32_t last_last = 0;
    uint64_t packets = 0;
    uint64_t samples = 0;
    uint64_t gaps = 0;
};

static std::array<RxStats, app_config::kMaxCanIds> s_rx_stats = {};
static TickType_t s_next_stats_log_tick = 0;

inline RxStats *FindOrCreateStats(uint32_t can_id)
{
    for (auto &stats : s_rx_stats) {
        if (stats.valid && stats.can_id == can_id) {
            return &stats;
        }
    }
    for (auto &stats : s_rx_stats) {
        if (!stats.valid) {
            stats.valid = true;
            stats.can_id = can_id;
            return &stats;
        }
    }
    return nullptr;
}

inline void LogStatsIfDue()
{
    const TickType_t now = xTaskGetTickCount();
    if (s_next_stats_log_tick == 0) {
        s_next_stats_log_tick = now + pdMS_TO_TICKS(1000);
        return;
    }
    if (static_cast<int32_t>(now - s_next_stats_log_tick) < 0) {
        return;
    }

    s_next_stats_log_tick = now + pdMS_TO_TICKS(1000);
    for (const auto &stats : s_rx_stats) {
        if (!stats.valid) {
            continue;
        }
        ESP_LOGI("RX_STATS", "id=0x%lx packets=%llu samples=%llu gaps=%llu last=[%lu..%lu]",
                 static_cast<unsigned long>(stats.can_id), static_cast<unsigned long long>(stats.packets),
                 static_cast<unsigned long long>(stats.samples), static_cast<unsigned long long>(stats.gaps),
                 static_cast<unsigned long>(stats.last_first), static_cast<unsigned long>(stats.last_last));
    }
}

inline void RecordPacketStats(const data_packet_t &recv_packet)
{
    if (!recv_packet.data || recv_packet.data_count == 0) {
        return;
    }

    RxStats *stats = FindOrCreateStats(recv_packet.can_id);
    if (stats == nullptr) {
        static bool s_stats_full_warned = false;
        if (!s_stats_full_warned) {
            s_stats_full_warned = true;
            ESP_LOGW("RX_STATS", "stats table full; increase app_config::kMaxCanIds");
        }
        return;
    }

    const uint32_t first = recv_packet.data[0];
    const uint32_t last = recv_packet.data[recv_packet.data_count - 1];
    if (stats->packets > 0 && first > stats->last_counter + 1) {
        stats->gaps += static_cast<uint64_t>(first - stats->last_counter - 1);
    }
    if (stats->packets == 0 || last > stats->last_counter) {
        stats->last_counter = last;
    }
    stats->last_first = first;
    stats->last_last = last;
    stats->packets++;
    stats->samples += recv_packet.data_count;

    LogStatsIfDue();
}

} // namespace receiver_app_detail

namespace receiver_app {

inline void SetupReceiver(bool filter_enabled, const uint32_t *filter_ids, size_t filter_count)
{
    static const char *TAG = "RECEIVER_APP";

    if (filter_enabled) {
        ESP_LOGI(TAG, "RECEIVER mode - active filter with %u CAN ID(s)", static_cast<unsigned>(filter_count));
        for (size_t i = 0; i < filter_count; ++i) {
            ESP_LOGI(TAG, "Receiver filter[%u] = 0x%lx", static_cast<unsigned>(i),
                     static_cast<unsigned long>(filter_ids[i]));
        }
        wcan_init(true, const_cast<uint32_t *>(filter_ids), filter_count, nullptr, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "RECEIVER mode - accepting all CAN IDs");
    wcan_init(false, nullptr, 0, nullptr, 0, 0);
}

} // namespace receiver_app
