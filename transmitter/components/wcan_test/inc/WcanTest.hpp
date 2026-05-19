#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "RampCanSensor.hpp"
#include "TransceiverBase.hpp"

namespace wcan_test {

enum class Role {
    kIdle,
    kSensor,
    kReceiver,
};

struct TestConfig {
    static constexpr size_t kMaxCanIds = 16;
    static constexpr int kDefaultSensorHz = 200;
    static constexpr uint32_t kDefaultSensorBaseCanId = 0x100;
    static constexpr size_t kDefaultSensorCanIdCount = 1;
    static constexpr uint32_t kDefaultWcanLingerMs = 100;
    static constexpr uint32_t kDefaultTestDurationMs = 30000;
    static constexpr uint32_t kDefaultHostWaitTimeMs = 5000;
    static constexpr int kMinSensorHz = 1;
    static constexpr int kMaxSensorHz = 10000;
    static constexpr uint32_t kMaxWcanLingerMs = 60000;
    static constexpr uint32_t kMaxTestDurationMs = 3600000;
    static constexpr uint32_t kMaxHostWaitTimeMs = 60000;
    static constexpr int kProtocolVersion = 1;
    static constexpr uint32_t kMaxCanId = 0x1FFFFFFF;
    static constexpr uint8_t kEspNowChannel = 1;

    Role role = Role::kIdle;
    int sensor_hz = kDefaultSensorHz;
    uint32_t sensor_base_can_id = kDefaultSensorBaseCanId;
    size_t sensor_can_id_count = kDefaultSensorCanIdCount;
    uint32_t linger_ms = kDefaultWcanLingerMs;
    uint32_t test_duration_ms = kDefaultTestDurationMs;
    uint32_t host_wait_time_ms = kDefaultHostWaitTimeMs;
    bool receiver_filter_enabled = false;
    std::array<uint32_t, kMaxCanIds> receiver_filter_ids = {};
    size_t receiver_filter_count = 0;
    char transport[16] = {};

    const char* role_name() const;
    std::vector<wcan::CANId_t> sensor_tx_ids() const;
    std::vector<wcan::CANId_t> receiver_rx_ids() const;
};

class UartTestProtocol {
public:
    static TestConfig wait_for_boot_config();
    static void wait_for_test_start();
    static void print_ready(Role role);
    static void print_abort(Role role, const char* reason);
};

class WcanTestSession {
public:
    explicit WcanTestSession(const TestConfig& config) : _config(config) {}

    void ready() const;
    void abort(const char* reason) const;
    void wait_idle_start() const;
    void run(wcan::TransceiverBase& transceiver, wcan_sensor::RampCanSensor* sensor) const;

private:
    uint32_t stop_drain_timeout_ms() const;

    const TestConfig& _config;
};

} // namespace wcan_test
