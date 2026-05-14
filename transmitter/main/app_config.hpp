#pragma once

#include <cstddef>
#include <cstdint>

namespace app_config {

static constexpr size_t kMaxCanIds = 16;
static constexpr int kDefaultSensorHz = 200;
static constexpr uint32_t kDefaultSensorBaseCanId = 0x100;
static constexpr size_t kDefaultSensorCanIdCount = 1;
static constexpr uint32_t kDefaultWcanLingerMs = 100;
static constexpr int kMinSensorHz = 1;
static constexpr int kMaxSensorHz = 10000;
static constexpr uint32_t kMaxWcanLingerMs = 60000;
static constexpr int kRuntimeConfigProtocolVersion = 1;

} // namespace app_config
