#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Return a bounded response for one ordinary DNS question. IPv6 and other
// types receive an empty answer, allowing clients to fall back to IPv4.
inline size_t captive_dns_reply(uint8_t* packet, size_t length, size_t capacity,
                                const void* ipv4) {
    if (length < 12 || length > capacity || (packet[2] & 0xf8) ||
        packet[4] != 0 || packet[5] != 1) return 0;
    size_t end = 12;
    while (end < length && packet[end]) {
        const auto label = packet[end++];
        if (label > 63 || label > length - end) return 0;
        end += label;
    }
    if (end >= length || length - end < 5) return 0;
    ++end;
    const unsigned type = (packet[end] << 8) | packet[end + 1];
    const unsigned klass = (packet[end + 2] << 8) | packet[end + 3];
    end += 4;
    const bool answer = type == 1 && klass == 1;
    if (answer && capacity - end < 16) return 0;
    packet[2] = 0x80 | (packet[2] & 1);
    packet[3] = 0x80;
    packet[6] = packet[8] = packet[9] = packet[10] = packet[11] = 0;
    packet[7] = answer ? 1 : 0;
    if (answer) {
        const uint8_t header[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        std::memcpy(packet + end, header, sizeof(header));
        std::memcpy(packet + end + sizeof(header), ipv4, 4);
        end += 16;
    }
    return end;
}
