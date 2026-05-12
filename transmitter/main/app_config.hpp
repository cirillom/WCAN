#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#ifndef SENSOR_HZ
#define SENSOR_HZ 200
#endif

#ifndef SENSOR_BASE_CAN_ID
#define SENSOR_BASE_CAN_ID 0x100
#endif

#ifndef SENSOR_CAN_ID_COUNT
#define SENSOR_CAN_ID_COUNT 1
#endif

#ifndef WCAN_LINGER_MS
#define WCAN_LINGER_MS 100
#endif

#if SENSOR_CAN_ID_COUNT < 1
#error "SENSOR_CAN_ID_COUNT must be at least 1"
#endif

#if SENSOR_CAN_ID_COUNT > 16
#error "SENSOR_CAN_ID_COUNT supports at most 16 CAN IDs"
#endif

#ifndef RECEIVER_FILTER_COUNT
#define RECEIVER_FILTER_COUNT -1
#endif

#ifndef RECEIVER_FILTER_ID_0
#define RECEIVER_FILTER_ID_0 0
#endif
#ifndef RECEIVER_FILTER_ID_1
#define RECEIVER_FILTER_ID_1 0
#endif
#ifndef RECEIVER_FILTER_ID_2
#define RECEIVER_FILTER_ID_2 0
#endif
#ifndef RECEIVER_FILTER_ID_3
#define RECEIVER_FILTER_ID_3 0
#endif
#ifndef RECEIVER_FILTER_ID_4
#define RECEIVER_FILTER_ID_4 0
#endif
#ifndef RECEIVER_FILTER_ID_5
#define RECEIVER_FILTER_ID_5 0
#endif
#ifndef RECEIVER_FILTER_ID_6
#define RECEIVER_FILTER_ID_6 0
#endif
#ifndef RECEIVER_FILTER_ID_7
#define RECEIVER_FILTER_ID_7 0
#endif
#ifndef RECEIVER_FILTER_ID_8
#define RECEIVER_FILTER_ID_8 0
#endif
#ifndef RECEIVER_FILTER_ID_9
#define RECEIVER_FILTER_ID_9 0
#endif
#ifndef RECEIVER_FILTER_ID_10
#define RECEIVER_FILTER_ID_10 0
#endif
#ifndef RECEIVER_FILTER_ID_11
#define RECEIVER_FILTER_ID_11 0
#endif
#ifndef RECEIVER_FILTER_ID_12
#define RECEIVER_FILTER_ID_12 0
#endif
#ifndef RECEIVER_FILTER_ID_13
#define RECEIVER_FILTER_ID_13 0
#endif
#ifndef RECEIVER_FILTER_ID_14
#define RECEIVER_FILTER_ID_14 0
#endif
#ifndef RECEIVER_FILTER_ID_15
#define RECEIVER_FILTER_ID_15 0
#endif

namespace app_config {

static constexpr int kSensorHz = SENSOR_HZ;
static constexpr uint32_t kSensorBaseCanId = SENSOR_BASE_CAN_ID;
static constexpr size_t kSensorCanIdCount = SENSOR_CAN_ID_COUNT;
static constexpr uint32_t kWcanLingerMs = WCAN_LINGER_MS;
static constexpr int kReceiverFilterCount = RECEIVER_FILTER_COUNT;
static constexpr size_t kMaxReceiverFilterIds = 16;

inline std::array<uint32_t, kSensorCanIdCount> SensorCanIds()
{
    std::array<uint32_t, kSensorCanIdCount> ids = {};
    for (size_t i = 0; i < ids.size(); ++i) {
        ids[i] = kSensorBaseCanId + static_cast<uint32_t>(i);
    }
    return ids;
}

inline bool ReceiverFilterEnabled()
{
    return kReceiverFilterCount >= 0;
}

inline size_t ReceiverFilterCount()
{
    return ReceiverFilterEnabled() ? static_cast<size_t>(kReceiverFilterCount) : 0;
}

inline const std::array<uint32_t, kMaxReceiverFilterIds> &ReceiverFilterStorage()
{
    static const std::array<uint32_t, kMaxReceiverFilterIds> ids = {
        RECEIVER_FILTER_ID_0,  RECEIVER_FILTER_ID_1,  RECEIVER_FILTER_ID_2,  RECEIVER_FILTER_ID_3,
        RECEIVER_FILTER_ID_4,  RECEIVER_FILTER_ID_5,  RECEIVER_FILTER_ID_6,  RECEIVER_FILTER_ID_7,
        RECEIVER_FILTER_ID_8,  RECEIVER_FILTER_ID_9,  RECEIVER_FILTER_ID_10, RECEIVER_FILTER_ID_11,
        RECEIVER_FILTER_ID_12, RECEIVER_FILTER_ID_13, RECEIVER_FILTER_ID_14, RECEIVER_FILTER_ID_15,
    };
    return ids;
}

inline uint32_t *ReceiverFilterIds()
{
    return const_cast<uint32_t *>(ReceiverFilterStorage().data());
}

} // namespace app_config
