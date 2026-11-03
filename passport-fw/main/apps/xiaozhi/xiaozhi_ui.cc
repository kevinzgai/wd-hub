// Xiaozhi: shared terminal conversation view and bounded UTF-8 replies.
#include "apps/xiaozhi/xiaozhi_ui.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "ui/pixel_scene.h"
#include "display_text.h"
#include "utf8_tail.h"
#include "app_fonts.h"
#include "wifi_manager.h"
#include "lvgl.h"
#include <esp_log.h>
#include <cstdio>

namespace xiaozhi {

namespace {

// 状态点颜色:与整机状态色语义一致(墨/纸/草/黄/红/天蓝)
#define kTextMain (UI_TEXT)
#define kTextMuted (UI_SUB)
#define kUserBlue (UI_SKY_DARK)
#define kGreenAccent (UI_GRASS_DARK)
#define kAmberAccent (UI_WARNING)
#define kRedAccent (UI_RED)

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_status_badge = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_msg_container = nullptr;
lv_obj_t *s_user_text_label = nullptr;
lv_obj_t *s_ai_text_label = nullptr;
lv_obj_t *s_hint_label = nullptr;
lv_obj_t *s_companion = nullptr;
State s_face_state = State::Idle;
lv_timer_t *s_volume_timer = nullptr;
lv_timer_t *s_status_timer = nullptr;
ui_pixel_battery_t s_battery{};
lv_obj_t *s_wifi_bars[3]{};
const char *s_default_hint = "确定说话 · 上下音量";

// 文本内容也只在持锁时读写，避免与 lv_label 生命周期脱节。
std::string s_user_text;
std::string s_ai_reply_buffer;

constexpr size_t kMaxTextBytes = 1200;  // up to 400 three-byte Han characters

void refresh_status_bar() {
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    auto &wifi = WifiManager::GetInstance();
    const bool connected = wifi.IsConnected();
    const int rssi = connected ? wifi.GetRssi() : -127;
    const int bars = !connected ? 0 : rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1;
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, connected ? UI_TEXT : UI_TEXT_DIM);
}

void draw_companion(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    const bool error = s_face_state == State::Error || s_face_state == State::Disconnected;
    const bool thinking = s_face_state == State::Thinking || s_face_state == State::Connecting;
    const bool listening = s_face_state == State::Listening;
    const uint32_t accent = error ? UI_DANGER : thinking ? UI_WARNING : UI_ACCENT;
    ink.box(14, 26, 96, 52, UI_SURFACE);
    ink.box(22, 18, 80, 68, UI_SURFACE);
    ink.frame(22, 18, 80, 68, UI_BORDER, 2);
    ink.box(14, 34, 8, 24, accent); ink.box(102, 34, 8, 24, accent);
    ink.box(60, 8, 4, 10, accent); ink.box(56, 4, 12, 6, accent);
    const int eye_y = thinking ? 38 : 42;
    ink.box(36, eye_y, 10, error ? 4 : 14, accent);
    ink.box(78, eye_y, 10, error ? 4 : 14, accent);
    if (s_face_state == State::Speaking) ink.frame(52, 62, 20, 12, accent, 3);
    else {
        ink.box(52, 68, 20, 3, accent);
        if (!error) { ink.box(48, 64, 4, 4, accent); ink.box(72, 64, 4, 4, accent); }
    }
    ink.box(34, 90, 56, 4, UI_BORDER);
    if (listening) {
        ink.box(4, 36, 3, 22, accent); ink.box(117, 36, 3, 22, accent);
    }
    if (thinking) for (int i = 0; i < 3; ++i) ink.box(50 + i * 10, 103, 4, 4, accent);
}

// Once a long reply scrolls, the half-hidden top line fades out in four
// pixel steps instead of being sliced by the container edge.
void draw_reading_fade(lv_event_t *event) {
    auto *box = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (lv_obj_get_scroll_y(box) <= 0) return;
    pixel_scene::Ink ink(event);
    static const lv_opa_t steps[] = {LV_OPA_COVER, LV_OPA_70, LV_OPA_40, LV_OPA_20};
    for (int i = 0; i < 4; ++i) {
        ink.paint.bg_opa = steps[i];
        ink.box(0, i * 4, lv_obj_get_width(box), 4, UI_BG);
    }
}

void show_companion(bool visible) {
    if (!s_companion) return;
    if (visible) {
        lv_obj_remove_flag(s_companion, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_msg_container, 12, 212);
        lv_obj_set_height(s_msg_container, 52);
    } else {
        lv_obj_add_flag(s_companion, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_msg_container, 12, 78);
        lv_obj_set_height(s_msg_container, 186);
    }
}

void conversation_layout() {
    show_companion(false);
    lv_obj_set_flex_align(s_msg_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                         LV_FLEX_ALIGN_START);
    lv_obj_set_style_text_align(s_ai_text_label, LV_TEXT_ALIGN_LEFT, 0);
}

// 滚动到消息区底部，保证最新一轮对话可见
void scroll_to_bottom_locked() {
    if (s_msg_container) {
        lv_obj_update_layout(s_msg_container);
        lv_obj_scroll_to_y(s_msg_container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// 所有"改文案"的操作都走这里：先加锁、再加一次指针校验。
// 返回 false 表示控件已销毁（应用正在退出），调用方静默丢弃即可。
bool begin_update(uint32_t timeout_ms) {
    if (!bsp_lvgl_lock(timeout_ms)) return false;
    if (s_screen == nullptr) {                      // 加锁期间被 ui_hide 销毁
        bsp_lvgl_unlock();
        return false;
    }
    return true;
}

} // namespace

void ui_init() {
    if (!bsp_lvgl_lock(1000)) return;

    if (s_volume_timer) {
        lv_timer_delete(s_volume_timer);
        s_volume_timer = nullptr;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    s_status_badge = nullptr;
    s_status_label = nullptr;
    s_msg_container = nullptr;
    s_user_text_label = nullptr;
    s_ai_text_label = nullptr;
    s_hint_label = nullptr;
    s_companion = nullptr;
    s_face_state = State::Idle;
    s_user_text.clear();
    s_ai_reply_buffer.clear();

    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    auto *header = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(header, 12, 18, s_wifi_bars);
    auto *title = ui_pixel_label(header, "小智 AI", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(header, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    refresh_status_bar();

    lv_obj_t *top_bar = lv_obj_create(s_screen);
    lv_obj_set_size(top_bar, 216, 28);
    lv_obj_set_pos(top_bar, 12, 42);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_hor(top_bar, 0, 0);
    lv_obj_set_style_pad_ver(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_badge = lv_obj_create(top_bar);
    lv_obj_set_size(s_status_badge, 9, 9);
    lv_obj_align(s_status_badge, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_status_badge, 0, 0);
    lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kAmberAccent), 0);
    lv_obj_set_style_border_width(s_status_badge, 0, 0);

    s_status_label = lv_label_create(top_bar);
    lv_obj_set_style_text_font(s_status_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(kTextMain), 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_status_label, 196, 20);
    lv_label_set_text(s_status_label, "准备中...");
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 15, 0);

    // Keep the conversation open and reserve the shared two-line key footer.
    s_msg_container = lv_obj_create(s_screen);
    lv_obj_set_size(s_msg_container, 216, 186);
    lv_obj_set_pos(s_msg_container, 12, 78);
    lv_obj_set_style_bg_color(s_msg_container, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_msg_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_msg_container, 0, 0);
    lv_obj_set_style_border_width(s_msg_container, 0, 0);
    lv_obj_set_style_border_color(s_msg_container, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_all(s_msg_container, 0, 0);
    lv_obj_set_style_pad_row(s_msg_container, 12, 0);
    lv_obj_set_style_text_line_space(s_msg_container, 4, 0);
    lv_obj_set_flex_flow(s_msg_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_msg_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_msg_container, draw_reading_fade, LV_EVENT_DRAW_POST, nullptr);

    s_user_text_label = lv_label_create(s_msg_container);
    lv_obj_set_width(s_user_text_label, 216);
    lv_obj_set_style_text_font(s_user_text_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_user_text_label, lv_color_hex(kUserBlue), 0);
    lv_label_set_long_mode(s_user_text_label, LV_LABEL_LONG_WRAP);
    s_user_text.clear();
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    lv_obj_add_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);

    s_ai_text_label = lv_label_create(s_msg_container);
    lv_obj_set_width(s_ai_text_label, 216);
    lv_obj_set_style_text_font(s_ai_text_label, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_ai_text_label, lv_color_hex(kTextMain), 0);
    lv_label_set_long_mode(s_ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ai_text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ai_text_label, "确定开始说话\n再按确定结束");

    s_companion = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_companion);
    lv_obj_set_pos(s_companion, 58, 88);
    lv_obj_set_size(s_companion, 124, 112);
    lv_obj_remove_flag(s_companion, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_companion, draw_companion, LV_EVENT_DRAW_MAIN, nullptr);
    show_companion(true);

    s_default_hint = "确定说话 · 上下音量";
    s_hint_label = ui_pixel_footer(s_screen, s_default_hint, "长按确定返回", &buddy_font_16);
    s_status_timer = lv_timer_create([](lv_timer_t *) {
        if (s_screen && app_registry_screen_is_on()) refresh_status_bar();
    }, 5000, nullptr);

    bsp_lvgl_unlock();
}

void ui_show() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen == nullptr) {
        bsp_lvgl_unlock();
        ui_init();
        if (!bsp_lvgl_lock(1000)) return;
    }
    if (s_screen) lv_screen_load(s_screen);
    bsp_lvgl_unlock();
}

void ui_hide() {
    if (!bsp_lvgl_lock(1000)) return;
    if (s_volume_timer) {
        lv_timer_delete(s_volume_timer);
        s_volume_timer = nullptr;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
        s_status_badge = nullptr;
        s_status_label = nullptr;
        s_msg_container = nullptr;
        s_user_text_label = nullptr;
        s_ai_text_label = nullptr;
        s_hint_label = nullptr;
        s_companion = nullptr;
        s_battery = {};
        for (auto &bar : s_wifi_bars) bar = nullptr;
    }
    std::string().swap(s_user_text);
    std::string().swap(s_ai_reply_buffer);
    bsp_lvgl_unlock();
}

void ui_set_state(State state) {
    if (!begin_update(200)) return;
    s_face_state = state;
    if (s_companion) lv_obj_invalidate(s_companion);
    s_default_hint = state == State::Listening ? "确定结束 · 上下音量" :
                     state == State::Speaking || state == State::Thinking ?
                     "确定打断 · 上下音量" : "确定说话 · 上下音量";
    ui_pixel_label_set_text(s_hint_label, s_default_hint);
    ui_pixel_text_color(s_status_label,
        state == State::Error || state == State::Disconnected ? UI_DANGER :
        state == State::Connecting || state == State::Thinking ? UI_WARNING : UI_TEXT);

    switch (state) {
    case State::Disconnected:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kRedAccent), 0);
        if (!WifiManager::GetInstance().IsConnected()) {
            lv_label_set_text(s_status_label, "Wi-Fi 未连接，请配网");
        } else {
            lv_label_set_text(s_status_label, "未连接 · 确定重试");
        }
        break;
    case State::Connecting:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kAmberAccent), 0);
        lv_label_set_text(s_status_label, "正在连接小智");
        break;
    case State::Idle:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kGreenAccent), 0);
        lv_label_set_text(s_status_label, "准备就绪");
        break;
    case State::Listening:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kUserBlue), 0);
        lv_label_set_text(s_status_label, "正在聆听");
        break;
    case State::Thinking:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kAmberAccent), 0);
        lv_label_set_text(s_status_label, "正在思考");
        break;
    case State::Speaking:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kGreenAccent), 0);
        lv_label_set_text(s_status_label, "正在回复");
        break;
    case State::Error:
        lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(kRedAccent), 0);
        lv_label_set_text(s_status_label, "服务异常 · 确定重试");
        break;
    }

    bsp_lvgl_unlock();
}

void ui_set_status_text(const std::string &text) {
    if (!begin_update(200)) return;
    lv_label_set_text(s_status_label, text.c_str());
    bsp_lvgl_unlock();
}

void ui_set_user_text(const std::string &text) {
    if (!begin_update(200)) return;
    conversation_layout();
    lv_obj_remove_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);
    s_user_text = "你说: ";
    passport::append_utf8_tail(s_user_text, passport::fold_display_text(text), kMaxTextBytes);
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_set_ai_text(const std::string &text) {
    if (!begin_update(200)) return;
    conversation_layout();
    const std::string folded = passport::fold_display_text(text);
    const auto tail = passport::utf8_tail(folded, kMaxTextBytes);
    s_ai_reply_buffer.assign(tail.data(), tail.size());
    std::string display = "小智: " + s_ai_reply_buffer;
    lv_label_set_text(s_ai_text_label, display.c_str());
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_append_ai_text(const std::string &text) {
    if (!begin_update(200)) return;
    conversation_layout();

    passport::append_utf8_tail(s_ai_reply_buffer, passport::fold_display_text(text), kMaxTextBytes);

    std::string display = "小智: " + s_ai_reply_buffer;
    lv_label_set_text(s_ai_text_label, display.c_str());
    scroll_to_bottom_locked();
    bsp_lvgl_unlock();
}

void ui_set_hint(const std::string &text) {
    if (!begin_update(200)) return;
    lv_label_set_text(s_hint_label, text.c_str());
    bsp_lvgl_unlock();
}

void ui_set_volume(unsigned percent) {
    if (!begin_update(200)) return;
    lv_label_set_text_fmt(s_hint_label, "音量 %u%%", percent);
    if (s_volume_timer) {
        lv_timer_reset(s_volume_timer);
    } else {
        s_volume_timer = lv_timer_create([](lv_timer_t *) {
            s_volume_timer = nullptr;
            if (s_hint_label) {
                lv_label_set_text(s_hint_label, s_default_hint);
            }
        }, 1200, nullptr);
        if (s_volume_timer) lv_timer_set_repeat_count(s_volume_timer, 1);
    }
    bsp_lvgl_unlock();
}

void ui_clear_text() {
    if (!begin_update(200)) return;
    s_ai_reply_buffer.clear();
    s_user_text.clear();
    lv_label_set_text(s_user_text_label, s_user_text.c_str());
    lv_obj_add_flag(s_user_text_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_ai_text_label, "");
    show_companion(true);
    bsp_lvgl_unlock();
}

} // namespace xiaozhi
