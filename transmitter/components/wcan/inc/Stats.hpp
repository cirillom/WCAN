#pragma once

#include <cstdint>
#include <memory>
#include <cstdio>

#include "Packet.hpp"

namespace wcan {

class Stats {
public:
    virtual ~Stats() = default;

    virtual void reset() {}
    virtual int64_t now_us() const { return 0; }
    virtual void record_rx_packet(const Packet&) {}
    virtual void record_airtime(uint32_t) {}
    virtual void record_batch(CANId_t, uint32_t, int64_t, uint32_t) {}
    virtual void record_sensor_send_failure() {}
    virtual void finish_test() {}
    virtual void print_sensor_end(uint32_t generated_count) const {
        if (generated_count == 0) {
            std::printf("WCAN_SENSOR_END generated=none avg_hz=0.00\n");
        } else {
            std::printf("WCAN_SENSOR_END generated=%lu avg_hz=0.00\n",
                        static_cast<unsigned long>(generated_count - 1));
        }
        std::fflush(stdout);
    }
    virtual void print_rx_ranges() const {}
    virtual void print_batch_stats() const {}
    virtual void print_measures() const {}
};

std::unique_ptr<Stats> make_stats();

} // namespace wcan
