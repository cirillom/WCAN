#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "TransceiverBase.hpp"

struct esp_timer;

namespace wcan_sensor {

class TimedSensor {
public:
    using Callback = void (*)(void*);

    ~TimedSensor();

    bool start(uint32_t hz, Callback callback, void* context, const char* name);
    void stop();
    bool running() const { return _timer != nullptr; }

private:
    esp_timer* _timer = nullptr;
};

class RampGenerator {
public:
    uint32_t next() { return _generated++; }
    uint32_t generated_count() const { return _generated; }
    void reset() { _generated = 0; }

private:
    uint32_t _generated = 0;
};

class RampCanSensor {
public:
    using SendFailureCallback = std::function<void(uint32_t can_id, uint32_t counter)>;

    explicit RampCanSensor(wcan::TransceiverBase& transceiver);
    ~RampCanSensor();

    bool start(uint32_t hz);
    void stop();
    uint32_t generated_count() const { return _generator.generated_count(); }
    void set_send_failure_callback(SendFailureCallback callback) { _send_failure_callback = std::move(callback); }

private:
    static void timer_callback(void* context);
    void tick();

    wcan::TransceiverBase& _transceiver;
    TimedSensor _timer;
    RampGenerator _generator;
    SendFailureCallback _send_failure_callback;
};

} // namespace wcan_sensor
