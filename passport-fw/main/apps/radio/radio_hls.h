#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>

// Bounded, allocation-free playlist parser. Kept independent of ESP-IDF for
// malformed-input and live-window tests on the host.
namespace radio_hls {
constexpr size_t kUrl = 512;
constexpr size_t kSegments = 6;
inline bool starts(const char *s, const char *prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}
inline bool resolve(const char *base, const char *ref, char *out, size_t cap) {
    if (!ref[0] || std::strchr(ref, '\r') || std::strchr(ref, '\n')) return false;
    int n = -1;
    const char *scheme = std::strstr(base, "://");
    if (!scheme) return false;
    if (starts(ref, "http://") || starts(ref, "https://")) n = std::snprintf(out, cap, "%s", ref);
    else if (std::strstr(ref, "://")) return false;
    else if (starts(ref, "//")) n = std::snprintf(out, cap, "%.*s:%s", int(scheme - base), base, ref);
    else {
        const char *path = std::strchr(scheme + 3, '/');
        if (ref[0] == '/') n = std::snprintf(out, cap, "%.*s%s", int(path ? path - base : std::strlen(base)), base, ref);
        else {
            const size_t end = std::strcspn(base, "?#");
            size_t slash = end;
            while (slash && base[slash - 1] != '/') --slash;
            if (!path) n = std::snprintf(out, cap, "%.*s/%s", int(end), base, ref);
            else n = std::snprintf(out, cap, "%.*s%s", int(slash), base, ref);
        }
    }
    return n > 0 && size_t(n) < cap;
}
struct Segment { char url[kUrl]{}; uint64_t sequence{}; bool discontinuity{}; };
struct Playlist {
    Segment segments[kSegments]{};
    char variant[kUrl]{};
    size_t count{};
    unsigned target{6};
    bool valid{}, failed{}, ended{}, master{}, media{};
    uint64_t sequence{}, minimum{};
    bool initial{}, pending_segment{}, pending_variant{}, discontinuity{}, truncated{};
    unsigned bandwidth{}, best_bandwidth{UINT_MAX};

    void reset(uint64_t next, bool first) {
        count = 0; variant[0] = 0; target = 6; best_bandwidth = UINT_MAX;
        valid = failed = ended = master = media = false;
        pending_segment = pending_variant = discontinuity = truncated = false;
        sequence = bandwidth = 0; minimum = next; initial = first;
    }
    void line(char *s, const char *base) {
        const size_t len = std::strlen(s);
        if (len && s[len - 1] == '\r') s[len - 1] = 0;
        if (!s[0] || failed) return;
        if (!valid) { valid = std::strcmp(s, "#EXTM3U") == 0; failed = !valid; return; }
        if (starts(s, "#EXT-X-KEY:") && std::strcmp(s, "#EXT-X-KEY:METHOD=NONE") != 0) { failed = true; return; }
        if (starts(s, "#EXT-X-MAP:") || starts(s, "#EXT-X-BYTERANGE:") || starts(s, "#EXT-X-GAP") || starts(s, "#EXT-X-I-FRAMES-ONLY")) { failed = true; return; }
        if (starts(s, "#EXT-X-MEDIA-SEQUENCE:")) {
            const char *number = s + 22;
            char *end = nullptr;
            sequence = std::strtoull(number, &end, 10);
            if (!number[0] || number[0] == '-' || *end || sequence > UINT64_MAX - 100000) failed = true;
        } else if (starts(s, "#EXT-X-TARGETDURATION:")) {
            const long value = std::strtol(s + 22, nullptr, 10);
            if (value < 1 || value > 60) failed = true;
            else target = unsigned(value);
        } else if (starts(s, "#EXT-X-STREAM-INF:")) {
            master = true;
            // First release only admits explicitly audio-only AAC/MP3 variants.
            const char *bw = std::strstr(s, "BANDWIDTH=");
            bandwidth = bw ? unsigned(std::strtoul(bw + 10, nullptr, 10)) : 0;
            pending_variant = bandwidth > 0 && bandwidth <= 192000 &&
                !std::strstr(s, "RESOLUTION=") && !std::strstr(s, "avc") &&
                !std::strstr(s, "hvc") && !std::strstr(s, "hev") &&
                !std::strstr(s, "av01") && !std::strstr(s, "vp09") &&
                (std::strstr(s, "mp4a.") || std::strstr(s, "mp3"));
        } else if (starts(s, "#EXTINF:")) { pending_segment = true; media = true; }
        else if (std::strcmp(s, "#EXT-X-DISCONTINUITY") == 0) discontinuity = true;
        else if (std::strcmp(s, "#EXT-X-ENDLIST") == 0) ended = true;
        else if (s[0] != '#') {
            if (pending_variant && bandwidth < best_bandwidth) {
                if (!resolve(base, s, variant, sizeof(variant))) failed = true;
                best_bandwidth = bandwidth;
            } else if (pending_segment) {
                if (sequence >= minimum) {
                    // Initial tune-in keeps the live edge; refresh keeps earliest
                    // not-yet-consumed segments so slow readers never replay data.
                    if (count == kSegments && initial) {
                        truncated = true;
                        std::memmove(segments, segments + 1, sizeof(Segment) * (kSegments - 1));
                        --count;
                    }
                    if (count < kSegments) {
                        auto &seg = segments[count++];
                        seg.sequence = sequence; seg.discontinuity = discontinuity;
                        if (!resolve(base, s, seg.url, sizeof(seg.url))) failed = true;
                    }
                }
                ++sequence;
            }
            pending_segment = pending_variant = discontinuity = false;
        }
    }
    bool complete() const { return valid && !failed && !(ended && truncated) && !(master && media) && !pending_segment && (master ? variant[0] != 0 : media); }
};
} // namespace radio_hls
