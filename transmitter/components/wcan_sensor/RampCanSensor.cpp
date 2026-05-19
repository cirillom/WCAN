#include "RampCanSensor.hpp"

#include <algorithm>

#include "esp_log.h"
#include "esp_timer.h"

namespace wcan_sensor {

static const char* TAG = "RAMP_SENSOR";

TimedSensor::~TimedSensor() {
    stop();
}

bool TimedSensor::start(uint32_t hz, Callback callback, void* context, const char* name) {
    stop();

    const uint32_t effective_hz = std::max<uint32_t>(1, hz);
    const uint64_t period_us = std::max<uint64_t>(1, 1000000ULL / static_cast<uint64_t>(effective_hz));

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = callback;
    timer_args.arg = context;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = name;
    timer_args.skip_unhandled_events = true;

    esp_err_t err = esp_timer_create(&timer_args, &_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create sensor timer: %s", esp_err_to_name(err));
        _timer = nullptr;
        return false;
    }

    err = esp_timer_start_periodic(_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sensor timer: %s", esp_err_to_name(err));
        stop();
        return false;
    }

    return true;
}

void TimedSensor::stop() {
    if (_timer == nullptr) {
        return;
    }
    esp_timer_stop(_timer);
    esp_timer_delete(_timer);
    _timer = nullptr;
}

RampCanSensor::RampCanSensor(wcan::TransceiverBase& transceiver)
    : _transceiver(transceiver) {}

RampCanSensor::~RampCanSensor() {
    stop();
}

bool RampCanSensor::start(uint32_t hz) {
    _generator.reset();
    return _timer.start(hz, timer_callback, this, "ramp_sensor");
}

void RampCanSensor::stop() {
    _timer.stop();
}

void RampCanSensor::timer_callback(void* context) {
    static_cast<RampCanSensor*>(context)->tick();
}

void RampCanSensor::tick() {
    const uint32_t counter = _generator.next();
    for (wcan::CANId_t can_id : _transceiver.get_tx_can_ids()) {
        if (!_transceiver.send_data(can_id, counter) && _send_failure_callback) {
            _send_failure_callback(can_id, counter);
        }
    }
}

} // namespace wcan_sensor
