#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#define private public
#include "apps/radio/radio_stream_buffer.h"
#undef private

static void stream_test(std::size_t capacity, bool wrap) {
    std::vector<uint8_t> storage(capacity);
    radio_stream::Buffer buffer(storage.data(), storage.size());
    if (wrap) buffer.read_ = buffer.write_ = UINT32_MAX - 111;
    constexpr std::size_t total = 1'000'000;
    std::thread producer([&] {
        std::mt19937 rng(7);
        std::size_t sent = 0;
        while (sent < total) {
            std::size_t room;
            auto *target = buffer.write_data(room);
            const auto bytes = std::min({room, std::size_t(rng() % 1024 + 1), total - sent});
            if (!bytes) { std::this_thread::yield(); continue; }
            for (std::size_t i = 0; i < bytes; ++i) target[i] = uint8_t((sent + i) * 37);
            buffer.commit(bytes);
            sent += bytes;
            // Network arrives in bursts independently of audio consumption.
            if (rng() % 25 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    uint8_t block[1117];
    std::size_t received = 0;
    while (received < total) {
        const auto bytes = buffer.read(block, sizeof(block));
        assert(buffer.available() <= capacity);
        if (!bytes) { std::this_thread::yield(); continue; }
        for (std::size_t i = 0; i < bytes; ++i) assert(block[i] == uint8_t((received + i) * 37));
        received += bytes;
    }
    producer.join();
    assert(buffer.available() == 0);
}

int main() {
    for (auto capacity : {8u * 1024, 16u * 1024}) {
        stream_test(capacity, false);
        stream_test(capacity, true);
    }
    std::vector<uint8_t> source, expected;
    for (unsigned section = 0; section < 80; ++section) {
        for (unsigned i = 0; i < 37; ++i) {
            source.push_back(uint8_t(i + section));
            expected.push_back(uint8_t(i + section));
        }
        const auto units = section % 4 == 3 ? 255 : section % 4;
        source.push_back(uint8_t(units));
        for (unsigned i = 0; i < units * 16; ++i) source.push_back(uint8_t(i));
    }
    for (std::size_t chunk = 1; chunk < 130; ++chunk) {
        radio_stream::IcyFilter filter(37);
        std::vector<uint8_t> actual;
        for (std::size_t i = 0; i < source.size(); i += chunk) {
            auto data = std::vector<uint8_t>(source.begin() + i, source.begin() + std::min(source.size(), i + chunk));
            const auto bytes = filter.filter(data.data(), data.size());
            actual.insert(actual.end(), data.begin(), data.begin() + bytes);
        }
        assert(actual == expected);
    }
    radio_stream::IcyFilter passthrough;
    auto data = source;
    assert(passthrough.filter(data.data(), data.size()) == data.size() && data == source);
    std::cout << "Radio FIFO concurrency, wraparound and fragmented ICY metadata: PASS\n";
}
