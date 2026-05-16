#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.hpp"
#include "wcan.hpp"
#include "wcan_utils.hpp"

namespace sensor_app {

struct SensorTaskConfig {
    std::array<uint32_t, app_config::kMaxCanIds> can_ids;
    std::array<size_t, app_config::kMaxCanIds> queue_indexes;
    size_t can_id_count;
    int sensor_hz;
};

static SensorTaskConfig g_sensor_task_config = {};
static constexpr UBaseType_t kReadDataTaskPriority = 10;

inline void read_data_task(void *pv_parameter)
{
    static const char *TAG = "read_data_task";
    auto *config = static_cast<SensorTaskConfig *>(pv_parameter);
    uint32_t counter = 0;

    ESP_LOGI(TAG, "read_data_task started with %u CAN ID(s) at %d Hz",
             static_cast<unsigned>(config->can_id_count), config->sensor_hz);

    const int hz = std::max(1, config->sensor_hz);
    const uint32_t period_ms = 1000 / hz;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        for (size_t i = 0; i < config->can_id_count; ++i) {
            const uint32_t can_id = config->can_ids[i];
            const size_t queue_index = config->queue_indexes[i];

            if (xQueueSend(can_queues[queue_index], &counter, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "[0x%lx] Send queue full, dropping counter=%lu",
                         static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            } else {
                std::printf("S:%lu:%lx:%lu\n", (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount()), 
                         static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            }
        }

        counter++;
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(period_ms));
    }
}

inline void SetupSensor(uint32_t base_can_id, size_t active_count, int sensor_hz, uint32_t linger_ms)
{
    static const char *TAG = "SENSOR_APP";

    g_sensor_task_config.can_id_count = active_count;
    g_sensor_task_config.sensor_hz = sensor_hz;
    for (size_t i = 0; i < active_count; ++i) {
        g_sensor_task_config.can_ids[i] = base_can_id + static_cast<uint32_t>(i);
    }

    ESP_LOGI(TAG, "SENSOR mode - CAN ID: 0x%lx (count=%u, linger=%lu ms, hz=%d)",
             static_cast<unsigned long>(g_sensor_task_config.can_ids[0]), static_cast<unsigned>(active_count),
             static_cast<unsigned long>(linger_ms), sensor_hz);

    for (size_t i = 0; i < active_count; ++i) {
        ESP_LOGI(TAG, "Sensor CAN ID[%u] = 0x%lx", static_cast<unsigned>(i),
                 static_cast<unsigned long>(g_sensor_task_config.can_ids[i]));
    }

    const uint32_t jitter_ms = esp_random() % 1000;
    ESP_LOGI(TAG, "Startup jitter: %lu ms", static_cast<unsigned long>(jitter_ms));
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    wcan_init(true, nullptr, 0, g_sensor_task_config.can_ids.data(), active_count, linger_ms);

    for (size_t i = 0; i < active_count; ++i) {
        const uint32_t can_id = g_sensor_task_config.can_ids[i];
        const size_t queue_index = get_can_tx_queue_index(can_id);
        if (queue_index == static_cast<size_t>(-1)) {
            ESP_LOGE(TAG, "CAN ID 0x%lx not found in WCAN queues", static_cast<unsigned long>(can_id));
            return;
        }
        g_sensor_task_config.queue_indexes[i] = queue_index;
    }

    xTaskCreate(read_data_task, "read_data_task", 4096, &g_sensor_task_config, kReadDataTaskPriority, nullptr);
}

} // namespace sensor_app
