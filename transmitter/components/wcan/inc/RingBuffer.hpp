#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace wcan {

template <typename T, size_t N>
struct RingBuffer {
    std::array<T, N> data;
    size_t write_idx = 0;
    size_t read_idx = 0;
    std::atomic<size_t> count{0};

    bool is_full() const { return count.load() >= N; }
    bool is_empty() const { return count.load() == 0; }

    T& write_head() { return data[write_idx]; }
    const T& write_head() const { return data[write_idx]; }
    T& read_head() { return data[read_idx]; }
    const T& read_head() const { return data[read_idx]; }

    void push() {
        write_idx = (write_idx + 1) % N;
        count.fetch_add(1);
    }
    void pop() {
        read_idx = (read_idx + 1) % N;
        count.fetch_sub(1);
    }
};

} // namespace wcan
