#include "launcher/network_diagnostics.h"
#include "launcher/app_registry.h"
#include "wifi_manager.h"
#include "passport_muse.h"
#include "apps/voice/time_sync.h"
#include "apps/home/home_weather.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "ui/ssid_text.h"
#include "lvgl.h"
#include <cstdio>
#include <ctime>
#include <cstdlib>

namespace {
lv_obj_t *s_screen, *s_ssid, *s_info;
lv_timer_t *s_timer;
void refresh() {
    auto &wifi = WifiManager::GetInstance();
    // Same subset Chinese font as the existing network picker; label is bounded.
    const auto ssid = passport::ssid_display(wifi.GetSsid(), &buddy_font_16);
    ui_pixel_label_set_text(s_ssid, wifi.IsConnected() ? ssid.c_str() : "未连接无线网络");
    const char *internet[] = {"未连接", "正在检查", "可以上网", "需网页登录", "无法上网"};
    char proxy[32], age[32] = "暂无天气", synced[24] = "未校时";
    uint16_t port = 0;
    const int route = passport_muse_proxy_route_info(proxy, sizeof(proxy), &port);
    const time_t last = time_sync_last_epoch() + time_sync_tz_offset_sec();
    if (time_sync_valid()) {
        struct tm tm{}; gmtime_r(&last, &tm);
        strftime(synced, sizeof(synced), "%m-%d %H:%M:%S", &tm);
    }
    home_weather_t weather{};
    if (home_weather_get(&weather) && weather.valid) home_weather_age_text(weather.updated_at, time_sync_valid() ? time(nullptr) : 0, age, sizeof(age));
    const int tz = time_sync_tz_offset_sec() / 60;
    char text[512];
    std::snprintf(text, sizeof(text), "RSSI %d dBm\nIP %s\n%s\nMuse %s\n%s:%u\n校时 %s\nUTC%c%02d:%02d\n天气 %s\n电量 %d%%", wifi.GetRssi(),
        wifi.IsConnected() ? wifi.GetIpAddress().c_str() : "--", internet[static_cast<int>(wifi.GetInternetStatus())],
        route == 1 ? "电脑代理" : "直连", route ? proxy : "--", port,
        synced, tz < 0 ? '-' : '+', std::abs(tz) / 60, std::abs(tz) % 60, age, bsp_battery_soc());
    ui_pixel_label_set_text(s_info, text);
}
}
bool network_diagnostics_active() { return s_screen != nullptr; }
void network_diagnostics_open() {
    bsp_lvgl_lock(-1);
    s_screen = lv_obj_create(nullptr); ui_pixel_background(s_screen);
    auto *title = ui_pixel_label(s_screen, "网络与时间", &buddy_font_16, UI_TEXT);
    lv_obj_set_pos(title, 12, 8);
    s_ssid = ui_pixel_label(s_screen, "", &buddy_font_16, UI_ACCENT);
    lv_obj_set_pos(s_ssid, 12, 36); lv_obj_set_width(s_ssid, 216); lv_label_set_long_mode(s_ssid, LV_LABEL_LONG_DOT);
    s_info = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT);
    lv_obj_set_pos(s_info, 12, 64); lv_obj_set_size(s_info, 216, 220); lv_obj_set_style_text_line_space(s_info, 4, 0);
    ui_pixel_footer(s_screen, "确定返回设置", "长按确定退出", &buddy_font_16);
    refresh(); s_timer = lv_timer_create([](lv_timer_t *) {
        if (app_registry_screen_is_on()) refresh();
    }, 2000, nullptr);
    lv_screen_load(s_screen); bsp_lvgl_unlock();
}
void network_diagnostics_close() {
    bsp_lvgl_lock(-1);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = s_ssid = s_info = nullptr; }
    bsp_lvgl_unlock();
}
