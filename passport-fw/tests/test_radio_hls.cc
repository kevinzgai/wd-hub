#include "apps/radio/radio_hls.h"
#include <cassert>
#include <string>
#include <cstdio>
using namespace radio_hls;
void feed(Playlist &p, const std::string &s, const char *base = "https://radio.test/live/index.m3u8?token=x") {
    size_t pos = 0;
    while (pos < s.size()) {
        auto end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        std::string line = s.substr(pos, end - pos);
        p.line(line.data(), base); pos = end + 1;
    }
}
int main() {
    Playlist p; p.reset(0, true);
    feed(p, "#EXTM3U\r\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:10,\nseg.aac?x=1\n#EXTINF:10,\n/next.ts\n");
    assert(p.complete() && p.count == 2 && p.target == 10);
    assert(p.segments[0].sequence == 100);
    assert(!std::strcmp(p.segments[0].url, "https://radio.test/live/seg.aac?x=1"));
    assert(!std::strcmp(p.segments[1].url, "https://radio.test/next.ts"));
    std::string window = "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:100\n";
    for (int i = 0; i < 20; ++i) window += "#EXTINF:6,\n" + std::to_string(i) + ".ts\n";
    p.reset(0, true); feed(p, window);
    assert(p.complete() && p.count == 6 && p.segments[0].sequence == 114);
    feed(p, "#EXT-X-ENDLIST\n"); assert(!p.complete()); // never silently truncate VOD
    p.reset(103, false); feed(p, window);
    assert(p.count == 6 && p.segments[0].sequence == 103 && p.segments[5].sequence == 108);
    p.reset(120, false); feed(p, window); assert(p.complete() && !p.count);
    p.reset(0, true);
    feed(p, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=32000,CODECS=\"avc1.42,mp4a.40.2\"\nvideo.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nhigh.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\n//cdn.test/low.m3u8\n");
    assert(p.complete() && !std::strcmp(p.variant, "https://cdn.test/low.m3u8"));
    for (const auto &tag : {"#EXT-X-KEY:METHOD=AES-128,URI=\"key\"", "#EXT-X-MAP:URI=\"init.mp4\"", "#EXT-X-BYTERANGE:100@0", "#EXT-X-GAP", "#EXT-X-MEDIA-SEQUENCE:-1", "#EXT-X-TARGETDURATION:0"}) {
        p.reset(0, true); feed(p, std::string("#EXTM3U\n") + tag + "\n#EXTINF:6,\na.ts\n"); assert(!p.complete());
    }
    p.reset(0, true); feed(p, "<html>error</html>"); assert(!p.complete());
    p.reset(0, true); feed(p, "#EXTM3U\n#EXTINF:6,\n"); assert(!p.complete());
    p.reset(0, true); feed(p, "#EXTM3U\n#EXTINF:6,\n" + std::string(900, 'x') + "\n"); assert(!p.complete());
    p.reset(0, true); feed(p, "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXT-X-DISCONTINUITY\n#EXTINF:6,\na.ts\n#EXTINF:6,\nb.ts\n#EXT-X-ENDLIST\n");
    assert(p.complete() && p.ended && p.segments[0].discontinuity && !p.segments[1].discontinuity);
    char url[100];
    assert(resolve("http://radio.test", "a.ts", url, sizeof(url)) && !strcmp(url, "http://radio.test/a.ts"));
    assert(!resolve("http://radio.test/a", "ftp://invalid/b", url, sizeof(url)));
    puts("HLS playlist: live windows, sequence, variants, relative URLs, bounded inputs, unsupported formats PASS");
}
