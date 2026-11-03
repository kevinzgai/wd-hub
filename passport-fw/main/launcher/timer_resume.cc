#include "launcher/timer_resume.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "bsp_display.h"
#include <sys/time.h>

namespace {
struct Snapshot {
    stopwatch_t watch;
    pomodoro_t clock;
    int64_t wall_us;
    uint32_t checksum, magic;
};
RTC_NOINIT_ATTR Snapshot s_saved;
constexpr uint32_t kMagic = 0x54494d32;
int64_t wall_us() {
    timeval tv{}; gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}
uint32_t checksum(const Snapshot &s) {
    uint32_t sum = 2166136261u;
    const auto *bytes = reinterpret_cast<const unsigned char *>(&s);
    for (size_t i = 0; i < offsetof(Snapshot, checksum); ++i) sum = (sum ^ bytes[i]) * 16777619u;
    return sum;
}
}
void timers_save_for_restart() {
    if (!bsp_lvgl_lock(-1)) return;
    s_saved = {};
    stopwatch_export_state(&s_saved.watch); pomodoro_export_state(&s_saved.clock);
    timer_snapshot(s_saved.watch, s_saved.clock, esp_timer_get_time());
    s_saved.wall_us = wall_us();
    s_saved.checksum = checksum(s_saved); s_saved.magic = kMagic;
    bsp_lvgl_unlock();
}
void timers_restore_after_restart() {
    const bool valid = esp_reset_reason() == ESP_RST_SW && s_saved.magic == kMagic && s_saved.checksum == checksum(s_saved);
    s_saved.magic = 0; // consume once; cold boot and watchdog resets never restore
    if (!valid) return;
    const int64_t gap = wall_us() - s_saved.wall_us;
    const uint64_t downtime = gap >= 0 && gap < 60000000 ? static_cast<uint64_t>(gap) : 0;
    timer_rebase(s_saved.watch, s_saved.clock, esp_timer_get_time(), downtime);
    stopwatch_import_state(s_saved.watch); pomodoro_import_state(s_saved.clock);
}
