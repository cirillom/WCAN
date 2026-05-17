#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.hpp"

namespace runtime_config {

enum class Role {
    kIdle,
    kSensor,
    kReceiver,
};

struct RuntimeConfig {
    Role role = Role::kIdle;
    int sensor_hz = app_config::kDefaultSensorHz;
    uint32_t sensor_base_can_id = app_config::kDefaultSensorBaseCanId;
    size_t sensor_can_id_count = app_config::kDefaultSensorCanIdCount;
    uint32_t linger_ms = app_config::kDefaultWcanLingerMs;
    bool receiver_filter_enabled = false;
    std::array<uint32_t, app_config::kMaxCanIds> receiver_filter_ids = {};
    size_t receiver_filter_count = 0;
    char transport[16] = {};
};

const char *RoleName(Role role);
RuntimeConfig WaitForBootConfig();

} // namespace runtime_config
