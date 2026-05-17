#pragma once

#include <array>
#include <cstdint>

#include "app_config.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wcan.hpp"

#ifdef MEASURE_INSTR
extern volatile uint64_t g_airtime_total_us;
extern volatile uint64_t g_packets_sent_total;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
inline void log_task_stats(void)
{
    const UBaseType_t n_tasks = uxTaskGetNumberOfTasks();
    if (n_tasks == 0) {
        return;
    }
    TaskStatus_t *task_status = static_cast<TaskStatus_t *>(
        pvPortMalloc(static_cast<size_t>(n_tasks) * sizeof(TaskStatus_t)));
    if (task_status == nullptr) {
        ESP_LOGW("MEASURE", "task stats: OOM (n_tasks=%u)", static_cast<unsigned>(n_tasks));
        return;
    }
    uint32_t total_runtime = 0;
    const UBaseType_t got = uxTaskGetSystemState(task_status, n_tasks, &total_runtime);
    for (UBaseType_t i = 0; i < got; i++) {
        const TaskStatus_t &t = task_status[i];
        const uint32_t hwm_bytes =
            static_cast<uint32_t>(t.usStackHighWaterMark) * sizeof(StackType_t);
        ESP_LOGI("TASK",
                 "name=%s prio=%lu state=%lu hwm_bytes=%lu runtime=%lu total_runtime=%lu",
                 t.pcTaskName,
                 static_cast<unsigned long>(t.uxCurrentPriority),
                 static_cast<unsigned long>(t.eCurrentState),
                 static_cast<unsigned long>(hwm_bytes),
                 static_cast<unsigned long>(t.ulRunTimeCounter),
                 static_cast<unsigned long>(total_runtime));
    }
    vPortFree(task_status);
}
#endif

inline void measure_periodic_task(void *)
{
    static const char *TAG = "MEASURE";
    static constexpr uint32_t MEASURE_LOG_INTERVAL_MS = 2500;
    const TickType_t period = pdMS_TO_TICKS(MEASURE_LOG_INTERVAL_MS);
    uint64_t prev_airtime_us = 0;
    uint64_t prev_packets = 0;

    while (true) {
        vTaskDelay(period);
        const uint64_t airtime = g_airtime_total_us;
        const uint64_t packets = g_packets_sent_total;
        const uint64_t d_airtime = airtime - prev_airtime_us;
        const uint64_t d_packets = packets - prev_packets;
        prev_airtime_us = airtime;
        prev_packets = packets;

        const uint64_t window_us = static_cast<uint64_t>(MEASURE_LOG_INTERVAL_MS) * 1000ULL;
        const uint32_t util_per_mille = window_us == 0 ? 0
            : static_cast<uint32_t>((d_airtime * 1000ULL) / window_us);

        ESP_LOGI(TAG,
                 "airtime_us_total=%llu packets_total=%llu d_airtime_us=%llu d_packets=%llu util_per_mille=%lu",
                 airtime, packets, d_airtime, d_packets, static_cast<unsigned long>(util_per_mille));

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
        log_task_stats();
#endif
    }
}
#endif

inline void heap_monitor_task(void *)
{
    static const char *TAG = "HEAP";
    static constexpr uint32_t WCAN_HEAP_MONITOR_INTERVAL_MS = 2500;
    const TickType_t period = pdMS_TO_TICKS(WCAN_HEAP_MONITOR_INTERVAL_MS);
    while (true) {
        ESP_LOGI(TAG, "free=%u min_free=%u largest=%u", static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        vTaskDelay(period);
    }
}

namespace app_stats_detail {

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

inline void RecordPacketStats(const wcan::Packet &recv_packet)
{
    const auto& data = recv_packet.get_data();
    if (data.empty()) {
        return;
    }

    RxStats *stats = FindOrCreateStats(recv_packet.get_can_id());
    if (stats == nullptr) {
        static bool s_stats_full_warned = false;
        if (!s_stats_full_warned) {
            s_stats_full_warned = true;
            ESP_LOGW("RX_STATS", "stats table full; increase app_config::kMaxCanIds");
        }
        return;
    }

    const uint32_t first = data.front();
    const uint32_t last = data.back();
    if (stats->packets > 0 && first > stats->last_counter + 1) {
        stats->gaps += static_cast<uint64_t>(first - stats->last_counter - 1);
    }
    if (stats->packets == 0 || last > stats->last_counter) {
        stats->last_counter = last;
    }
    stats->last_first = first;
    stats->last_last = last;
    stats->packets++;
    stats->samples += data.size();

    LogStatsIfDue();
}

} // namespace app_stats_detail

inline void start_app_stats(void)
{
    xTaskCreate(heap_monitor_task, "heap_monitor", 2048, nullptr, 1, nullptr);
#ifdef MEASURE_INSTR
    xTaskCreate(measure_periodic_task, "measure", 3072, nullptr, 1, nullptr);
#endif
}
