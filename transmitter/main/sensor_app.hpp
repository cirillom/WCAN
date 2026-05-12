#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.hpp"
#include "wcan.hpp"
#include "wcan_utils.hpp"

namespace sensor_app {

struct SensorTaskConfig {
    std::array<uint32_t, app_config::kSensorCanIdCount> can_ids;
    std::array<size_t, app_config::kSensorCanIdCount> queue_indexes;
    size_t can_id_count;
};

static SensorTaskConfig g_sensor_task_config = {};

inline TickType_t SensorPeriodTicks()
{
    const int period_ms = std::max(1, 1000 / app_config::kSensorHz);
    return pdMS_TO_TICKS(period_ms);
}

inline void read_data_task(void *pv_parameter)
{
    static const char *TAG = "read_data_task";
    auto *config = static_cast<SensorTaskConfig *>(pv_parameter);
    uint32_t counter = 0;

    ESP_LOGI(TAG, "read_data_task started with %u CAN ID(s) at %d Hz",
             static_cast<unsigned>(config->can_id_count), app_config::kSensorHz);

    while (true) {
        for (size_t i = 0; i < config->can_id_count; ++i) {
            const uint32_t can_id = config->can_ids[i];
            const size_t queue_index = config->queue_indexes[i];

            if (xQueueSend(can_queues[queue_index], &counter, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "[0x%lx] Send queue full, dropping counter=%lu",
                         static_cast<unsigned long>(can_id), static_cast<unsigned long>(counter));
            } else {
                ESP_LOGI(TAG, "[0x%lx] %lu", static_cast<unsigned long>(can_id),
                         static_cast<unsigned long>(counter));
            }
        }

        counter++;
        vTaskDelay(SensorPeriodTicks());
    }
}

template <size_t N>
inline void SetupSensor(const std::array<uint32_t, N> &can_ids, size_t active_count)
{
    static const char *TAG = "SENSOR_APP";

    g_sensor_task_config.can_ids = can_ids;
    g_sensor_task_config.can_id_count = active_count;

    ESP_LOGI(TAG, "SENSOR mode - CAN ID: 0x%lx (count=%u, linger=%lu ms, hz=%d)",
             static_cast<unsigned long>(can_ids[0]), static_cast<unsigned>(active_count),
             static_cast<unsigned long>(app_config::kWcanLingerMs), app_config::kSensorHz);

    for (size_t i = 0; i < active_count; ++i) {
        ESP_LOGI(TAG, "Sensor CAN ID[%u] = 0x%lx", static_cast<unsigned>(i),
                 static_cast<unsigned long>(can_ids[i]));
    }

    const uint32_t jitter_ms = esp_random() % 1000;
    ESP_LOGI(TAG, "Startup jitter: %lu ms", static_cast<unsigned long>(jitter_ms));
    vTaskDelay(pdMS_TO_TICKS(jitter_ms));

    wcan_init(true, nullptr, 0, g_sensor_task_config.can_ids.data(), active_count, app_config::kWcanLingerMs);

    for (size_t i = 0; i < active_count; ++i) {
        const uint32_t can_id = can_ids[i];
        const size_t queue_index = get_can_tx_queue_index(can_id);
        if (queue_index == static_cast<size_t>(-1)) {
            ESP_LOGE(TAG, "CAN ID 0x%lx not found in WCAN queues", static_cast<unsigned long>(can_id));
            return;
        }
        g_sensor_task_config.queue_indexes[i] = queue_index;
    }

    xTaskCreate(read_data_task, "read_data_task", 4096, &g_sensor_task_config, 5, nullptr);
}

inline void SetupSingleCanIdSensor()
{
    SetupSensor(app_config::SensorCanIds(), 1);
}

inline void SetupMultipleCanIdSensor()
{
    SetupSensor(app_config::SensorCanIds(), app_config::kSensorCanIdCount);
}

} // namespace sensor_app
