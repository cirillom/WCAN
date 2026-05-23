#pragma once

#include <cstdint>
#include <memory>
#include <cstdio>
#include <vector>

#include "Packet.hpp"

namespace wcan {

class Stats {
public:
    virtual ~Stats() = default;

    virtual void reset() {}
    virtual int64_t now_us() const { return 0; }
    virtual void record_rx_packet(const Packet&) {}
    virtual void record_airtime(uint32_t) {}
    virtual void record_batch(const Packet&, int64_t) {}
    virtual void record_batch_dispatch(const Packet&, int64_t, uint32_t) {}
    virtual void record_sensor_send_failure(CANId_t can_id, uint32_t first, uint32_t last) {}
    virtual void finish_test() {}
    // Optional hook to provide configured TX CAN IDs so implementations may
    // pre-allocate per-CAN slots to avoid concurrent unordered_map mutations.
    virtual void configure_tx_ids(const std::vector<CANId_t>&) {}
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
