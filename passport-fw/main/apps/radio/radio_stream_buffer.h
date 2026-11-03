#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace radio_stream {
// Single producer (HTTP) / single consumer (decoder). Positions are monotonically
// increasing unsigned counters; subtraction remains valid across wraparound.
// Storage is owned by the session and outlives both workers.
class Buffer {
public:
    Buffer(uint8_t *storage, std::size_t capacity) : data_(storage), capacity_(capacity) {
        assert(storage && capacity && !(capacity & (capacity - 1)));
    }
    std::size_t available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    uint8_t *write_data(std::size_t &contiguous) {
        const auto written = write_.load(std::memory_order_relaxed);
        const auto used = written - read_.load(std::memory_order_acquire);
        const auto offset = written % capacity_;
        contiguous = std::min<std::size_t>(capacity_ - used, capacity_ - offset);
        return data_ + offset;
    }
    void commit(std::size_t bytes) {
        write_.fetch_add(static_cast<uint32_t>(bytes), std::memory_order_release);
    }
    std::size_t read(uint8_t *target, std::size_t capacity) {
        const auto consumed = read_.load(std::memory_order_relaxed);
        const auto bytes = std::min<std::size_t>(capacity,
            write_.load(std::memory_order_acquire) - consumed);
        const auto offset = consumed % capacity_;
        const auto first = std::min<std::size_t>(bytes, capacity_ - offset);
        std::memcpy(target, data_ + offset, first);
        std::memcpy(target + first, data_, bytes - first);
        read_.store(consumed + static_cast<uint32_t>(bytes), std::memory_order_release);
        return bytes;
    }
private:
    uint8_t *data_;
    const std::size_t capacity_;
    std::atomic<uint32_t> read_{0}, write_{0};
};

// Some broadcasters send ICY metadata even when the request opts out. Strip it
// before MP3 parsing, including length/data split across reads or FIFO wraps.
class IcyFilter {
public:
    explicit IcyFilter(uint32_t interval = 0) : interval_(interval), audio_left_(interval) {}
    std::size_t filter(uint8_t *data, std::size_t bytes) {
        if (!interval_) return bytes;
        std::size_t out = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            if (metadata_left_) {
                if (--metadata_left_ == 0) audio_left_ = interval_;
            } else if (audio_left_) {
                data[out++] = data[i];
                --audio_left_;
            } else {
                metadata_left_ = static_cast<uint32_t>(data[i]) * 16;
                if (!metadata_left_) audio_left_ = interval_;
            }
        }
        return out;
    }
private:
    const uint32_t interval_;
    uint32_t audio_left_, metadata_left_{0};
};
}  // namespace radio_stream
