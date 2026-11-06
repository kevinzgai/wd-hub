// Wi-Fi 配网界面。原先与已下架的电台主屏共用一套 LVGL screen，独立后这里
// 只保留配网所需的页面与状态，绘制逻辑、坐标、文案与原实现保持一致，
// 避免改动既有配网交互和视觉。
#include "launcher/wifi_picker_ui.h"

#include "app_fonts.h"
#include "bsp_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "launcher/app_registry.h"
#include "lvgl.h"
#include "ui/ssid_text.h"
#include "ui_pixel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

#define kBackground (UI_BG)
#define kPanel (UI_SURFACE)
#define kPanelSoft (UI_SURFACE_ALT)
#define kGrid (UI_BORDER)
#define kText (UI_TEXT)
#define kMuted (UI_TEXT_DIM)
#define kAmber (UI_AMBER)
#define kAmberSoft (UI_SURFACE_ALT)
#define kRed (UI_RED)

constexpr std::size_t kWifiVisibleRows = 6;
constexpr std::size_t kWifiMaxKeys = 35;
constexpr std::size_t kSettingsRows = 8;

enum class Page {
    WifiList,
    WifiKeyboard,
    WifiConnecting,
    SavedWifi,
    WifiDeleteConfirm,
};

lv_obj_t *s_screen;
lv_timer_t *s_ui_timer;
Page s_page = Page::WifiList;
uint8_t s_animation;

lv_obj_t *s_pair_dot;
lv_obj_t *s_wifi_state;
lv_obj_t *s_wifi_list_hint;
lv_obj_t *s_wifi_row_box[kWifiVisibleRows];
lv_obj_t *s_wifi_row_ssid[kWifiVisibleRows];
lv_obj_t *s_wifi_row_bars[kWifiVisibleRows][3];
lv_obj_t *s_wifi_keyboard_network;
lv_obj_t *s_wifi_password_mask;
lv_obj_t *s_wifi_keyboard_status;
lv_obj_t *s_wifi_key_box[kWifiMaxKeys];
lv_obj_t *s_wifi_key_label[kWifiMaxKeys];
lv_obj_t *s_settings_rows[kSettingsRows];
lv_obj_t *s_settings_labels[kSettingsRows];
std::size_t s_settings_selected;
std::size_t s_settings_row_count;

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   uint32_t color, int radius = 0) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    (void)radius;
    lv_obj_set_style_radius(object, 0, 0);
    if (color == UI_SURFACE || color == UI_SURFACE_ALT) {
        lv_obj_set_style_border_width(object, ui_theme_is_classic() ? 1 : 0, 0);
        lv_obj_set_style_border_color(object, lv_color_hex(UI_BORDER), 0);
    }
    // 单核 ESP32-C3 上分批让出 CPU，避免一次完整构建占用超过任务看门狗窗口。
    vTaskDelay(1);
    return object;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                     uint32_t color, const lv_font_t *font = &buddy_font_16) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    vTaskDelay(1);
    return object;
}

void set_centered(lv_obj_t *object) {
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
}

static inline lv_obj_t *make_taskbar(lv_obj_t *parent) {
    (void)parent;
    return nullptr;
}

const lv_font_t *ssid_font(const char *ssid) {
    (void)ssid;
    return &buddy_font_16;
}

void reset_transient_objects(Page page) {
    lv_obj_clean(s_screen);
    s_page = page;
    s_pair_dot = nullptr;
    s_wifi_state = nullptr;
    s_wifi_list_hint = nullptr;
    s_wifi_keyboard_network = nullptr;
    s_wifi_password_mask = nullptr;
    s_wifi_keyboard_status = nullptr;
    for (auto &box : s_wifi_row_box) box = nullptr;
    for (auto &label : s_wifi_row_ssid) label = nullptr;
    for (auto &row_bars : s_wifi_row_bars) {
        for (auto &bar : row_bars) bar = nullptr;
    }
    for (auto &box : s_wifi_key_box) box = nullptr;
    for (auto &label : s_wifi_key_label) label = nullptr;
    for (auto &row : s_settings_rows) row = nullptr;
    for (auto &label : s_settings_labels) label = nullptr;
}

void apply_settings_selection() {
    for (std::size_t i = 0; i < s_settings_row_count; ++i) {
        if (!s_settings_rows[i] || !s_settings_labels[i]) continue;
        const bool active = i == s_settings_selected;
        ui_pixel_set_selected(s_settings_rows[i], active, true);
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        lv_obj_set_style_text_color(s_settings_labels[i],
                                    lv_color_hex(kText), 0);
    }
}

void build_saved_wifi(const char *const *ssids, std::size_t count,
                      std::size_t selected) {
    reset_transient_objects(Page::SavedWifi);
    const std::size_t total = count + 1;
    selected = std::min(selected, total - 1);

    lv_obj_t *title = make_label(s_screen, "已存网络", 12, 12, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    lv_obj_t *state = make_label(s_screen,
                                  count ? "上下选择  确定设置" : "无保存网络",
                                 12, 40, 216, count ? kMuted : kAmber);
    set_centered(state);

    const std::size_t first = total <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             total - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        if (index >= total) break;
        const bool active = index == selected;
        const int y = 67 + static_cast<int>(row) * 34;
        s_settings_rows[row] = make_box(s_screen, 18, y, 204, 29,
                                        active ? kAmberSoft : kPanel, 5);
        ui_pixel_set_selected(s_settings_rows[row], active, true);
        const char *text = index < count ? ssids[index] : "返回";
        s_settings_labels[row] = make_label(
            s_settings_rows[row], passport::ssid_display(text, &buddy_font_16).c_str(), 8, 6, 188, active ? UI_AMBER : kMuted,
            index < count ? ssid_font(text) : &buddy_font_16);
        set_centered(s_settings_labels[row]);
    }
    make_taskbar(s_screen);
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 291, 216, UI_TEXT);
    set_centered(hint);
}

void build_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::WifiDeleteConfirm);
    s_settings_selected = selected % 2;
    s_settings_row_count = 2;
    lv_obj_t *title = make_label(s_screen, "确认删除", 12, 18, 216, kText,
                                 &buddy_font_16);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str(), 12, 55, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    lv_obj_t *warning = make_label(s_screen, "删除后无法自动连接", 12, 88, 216,
                                   kMuted);
    set_centered(warning);
    static constexpr const char *kItems[] = {"返回", "确认删除"};
    for (std::size_t i = 0; i < 2; ++i) {
        const int y = 132 + static_cast<int>(i) * 61;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 45, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 13,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
}

void update_wifi_list(const WifiNetworkView *networks, std::size_t count,
                      std::size_t selected, bool scanning, bool can_cancel) {
    const char *state_text = scanning ? "正在寻找网络" :
                             count ? "选择网络后输入密码" : "未找到网络 · 确定重试";
    lv_label_set_text(s_wifi_state, state_text);
    lv_obj_set_style_text_color(s_wifi_state,
                                lv_color_hex(scanning ? kAmber : kMuted), 0);
    if (s_wifi_list_hint) {
        lv_label_set_text(s_wifi_list_hint,
                          can_cancel ? "上下选择 · 长按确定返回" : "上下选择 · 确定连接");
    }

    const std::size_t first = count <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             count - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        const bool present = index < count;
        const bool active = present && index == selected;
        ui_pixel_set_selected(s_wifi_row_box[row], active, true);
        lv_obj_set_style_bg_color(s_wifi_row_box[row],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        if (!present) {
            lv_label_set_text(s_wifi_row_ssid[row], "");
            for (int b = 0; b < 3; ++b) {
                lv_obj_set_style_bg_opa(s_wifi_row_bars[row][b], LV_OPA_0, 0);
            }
            continue;
        }

        const uint32_t color = active ? UI_AMBER : kMuted;
        char ssid_disp[34 + 2];
        std::snprintf(ssid_disp, sizeof(ssid_disp), "%s%s",
                      networks[index].secure ? "*" : "", passport::ssid_display(networks[index].ssid, &buddy_font_16).c_str());
        lv_label_set_text(s_wifi_row_ssid[row], ssid_disp);
        lv_obj_set_style_text_font(s_wifi_row_ssid[row],
                                   ssid_font(networks[index].ssid), 0);
        lv_obj_set_style_text_color(s_wifi_row_ssid[row], lv_color_hex(color), 0);
        // 右侧 "WiFi 信号" 三格：RSSI 分档与首页一致(-60/-75)
        const int rssi = networks[index].rssi;
        const int level = rssi == -128 ? 0 : rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        for (int b = 0; b < 3; ++b) {
            lv_obj_set_style_bg_color(s_wifi_row_bars[row][b],
                                      lv_color_hex(active ? kGrid : kMuted), 0);
            lv_obj_set_style_bg_opa(s_wifi_row_bars[row][b],
                                    rssi == -128 ? LV_OPA_0 : b < level ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

void build_wifi_list(const WifiNetworkView *networks, std::size_t count,
                     std::size_t selected, bool scanning, const char *setup_ap_ssid,
                     bool can_cancel) {
    reset_transient_objects(Page::WifiList);
    // The compact UI font contains the complete setup vocabulary. The larger
    // station-title font intentionally contains only station-name glyphs.
    make_label(s_screen, "无线网络", 12, 12, 216, kText, &ui_font_20);
    s_wifi_state = make_label(s_screen, "", 12, 42, 216, kMuted);
    set_centered(s_wifi_state);

    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const int y = 68 + static_cast<int>(row) * 34;
        s_wifi_row_box[row] = make_box(s_screen, 12, y, 216, 29, kPanel, 5);
        s_wifi_row_ssid[row] = make_label(s_wifi_row_box[row], "", 9, 6, 172,
                                          kMuted, &buddy_font_16);
        // 右侧 "WiFi 信号" 三格条：与首页/桌面顶栏同一视觉语言
        for (int b = 0; b < 3; ++b) {
            const int h = 5 + b * 4;
            s_wifi_row_bars[row][b] =
                make_box(s_wifi_row_box[row], 190 + b * 6, 22 - h, 4, h, kMuted, 1);
        }
    }

    make_taskbar(s_screen);
    s_wifi_list_hint = make_label(s_screen, "", 12, 279, 216, UI_TEXT);
    set_centered(s_wifi_list_hint);
    char portal[96];
    if (setup_ap_ssid && setup_ap_ssid[0]) std::snprintf(portal, sizeof(portal), "手机连 %s", setup_ap_ssid);
    else std::snprintf(portal, sizeof(portal), "长按下返回");
    lv_obj_t *hint = make_label(s_screen, portal, 12, 300, 216, UI_MUTED,
                                &buddy_font_16);
    set_centered(hint);
    update_wifi_list(networks, count, selected, scanning, can_cancel);
}

void update_wifi_keyboard(const char *ssid, std::size_t password_length,
                          WifiKeyboardPage page, std::size_t selected,
                          const char *status) {
    lv_label_set_text(s_wifi_keyboard_network, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str());
    lv_obj_set_style_text_font(s_wifi_keyboard_network, ssid_font(ssid), 0);

    char password[48] = {};
    const std::size_t stars = std::min<std::size_t>(password_length, 38);
    std::memset(password, '*', stars);
    password[stars] = '\0';
    lv_label_set_text(s_wifi_password_mask, password_length ? password : "输入密码");
    lv_obj_set_style_text_color(s_wifi_password_mask,
                                lv_color_hex(password_length ? kText : kMuted), 0);

    const std::size_t key_count = wifi_picker_ui_wifi_key_count(page);
    if (key_count > 0) selected %= key_count;
    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        if (i >= key_count) {
            lv_obj_add_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = i == selected;
        lv_obj_set_style_bg_color(s_wifi_key_box[i],
                                  lv_color_hex(active ? UI_AMBER : kPanelSoft), 0);
        lv_obj_set_style_border_width(s_wifi_key_box[i], active ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_wifi_key_box[i],
                                      lv_color_hex(active ? UI_TEXT : UI_BORDER), 0);
        const char *value = wifi_picker_ui_wifi_key(page, i);
        lv_label_set_text(s_wifi_key_label[i], std::strcmp(value, " ") == 0 ? "SP" :
            std::strcmp(value, "DEL") == 0 ? "BS" : value);
        lv_obj_set_style_text_color(s_wifi_key_label[i],
                                    lv_color_hex(active ? UI_BG : kText), 0);
    }

    char navigation[112];
    const char *selected_key = key_count > 0 ? wifi_picker_ui_wifi_key(page, selected) : "";
    const char *action = std::strcmp(selected_key, " ") == 0 ? "确定输入空格" :
        std::strcmp(selected_key, "DEL") == 0 ? (password_length ? "确定删除一位" : "确定返回网络列表") :
        std::strcmp(selected_key, "GO") == 0 ? "确定连接网络" :
        std::strcmp(selected_key, "ABC") == 0 ? "确定切换大写字母" :
        std::strcmp(selected_key, "abc") == 0 ? "确定切换小写字母" :
        std::strcmp(selected_key, "123") == 0 ? "确定切换数字符号" :
        std::strcmp(selected_key, "#+=") == 0 ? "确定查看更多符号" : nullptr;
    if (action) std::snprintf(navigation, sizeof(navigation), "%s", action);
    else std::snprintf(navigation, sizeof(navigation), "确定输入 %s · 已输 %u 位",
                       selected_key, (unsigned)password_length);
    lv_label_set_text(s_wifi_keyboard_status,
                      status && status[0] ? status : navigation);
    lv_obj_set_style_text_color(s_wifi_keyboard_status,
                                lv_color_hex(status && status[0] ? kRed : UI_TEXT), 0);
}

void build_wifi_keyboard(const char *ssid, std::size_t password_length,
                         WifiKeyboardPage page, std::size_t selected,
                         const char *status) {
    reset_transient_objects(Page::WifiKeyboard);
    make_label(s_screen, "输入密码", 12, 10, 216, kText, &ui_font_20);
    s_wifi_keyboard_network = make_label(s_screen, "", 12, 39, 216, kAmber,
                                          &buddy_font_16);
    set_centered(s_wifi_keyboard_network);

    lv_obj_t *password_box = make_box(s_screen, 18, 66, 204, 34, kPanel, 6);
    s_wifi_password_mask = make_label(password_box, "输入密码", 8, 8, 188, kMuted,
                                      &buddy_font_16);
    set_centered(s_wifi_password_mask);

    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        constexpr int kColumns = 6;
        const int x = 15 + static_cast<int>(i % kColumns) * 35;
        const int y = 109 + static_cast<int>(i / kColumns) * 28;
        // One styled label per key keeps the keyboard within the 24 KB LVGL
        // heap. A separate box + label for every key exhausted that heap near
        // key 31 and caused an immediate Store access fault.
        s_wifi_key_label[i] = make_label(s_screen, "", x, y, 31, kText,
                                         &lv_font_montserrat_14);
        s_wifi_key_box[i] = s_wifi_key_label[i];
        lv_obj_set_size(s_wifi_key_label[i], 31, 24);
        lv_obj_set_style_bg_color(s_wifi_key_label[i], lv_color_hex(kPanelSoft), 0);
        lv_obj_set_style_bg_opa(s_wifi_key_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_key_label[i], 0, 0);
        lv_obj_set_style_pad_top(s_wifi_key_label[i], 4, 0);
        set_centered(s_wifi_key_label[i]);
    }

    make_taskbar(s_screen);
    s_wifi_keyboard_status = make_label(s_screen, "", 12, 279, 216, UI_TEXT);
    set_centered(s_wifi_keyboard_status);
    lv_obj_t *hint = make_label(s_screen, "上下移动 · 长按下返回", 12, 300, 216, UI_MUTED);
    set_centered(hint);
    update_wifi_keyboard(ssid, password_length, page, selected, status);
}

void build_wifi_connecting(const char *ssid, const char *detail, bool success = false) {
    reset_transient_objects(Page::WifiConnecting);
    make_label(s_screen, success ? "无线网络已连接" : "正在连接无线网络", 12, 37, 216, kText,
               &buddy_font_16);
    lv_obj_t *network = make_label(s_screen, passport::ssid_display(ssid ? ssid : "WIFI", &buddy_font_16).c_str(), 12, 102, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    s_pair_dot = make_box(s_screen, 108, 151, 24, 24, kAmber, LV_RADIUS_CIRCLE);
    if (success) lv_obj_add_flag(s_pair_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *message = make_label(s_screen, detail ? detail : "正在连接",
                                   12, 205, 216, kMuted);
    set_centered(message);
}

void timer_callback(lv_timer_t *timer) {
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? 50 : 1000);
    if (!visible) return;
    s_animation += 17;
    if (s_page == Page::WifiConnecting && s_pair_dot) {
        const uint8_t triangle = s_animation < 128 ? s_animation : 255 - s_animation;
        lv_obj_set_style_opa(s_pair_dot, 70 + triangle, 0);
    }
}

}  // namespace

bool wifi_picker_ui_init() {
    if (s_screen) return true;
    if (!bsp_lvgl_lock(1000)) return false;
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    s_ui_timer = lv_timer_create(timer_callback, 50, nullptr);
    if (s_ui_timer) lv_timer_pause(s_ui_timer);
    bsp_lvgl_unlock();
    return true;
}

void wifi_picker_ui_deinit() {
    if (!bsp_lvgl_lock(-1)) return;
    if (s_ui_timer) { lv_timer_delete(s_ui_timer); s_ui_timer = nullptr; }
    if (s_screen) { reset_transient_objects(Page::WifiList); lv_obj_delete(s_screen); s_screen = nullptr; }
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_wifi_list(const WifiNetworkView *networks,
                                   std::size_t count, std::size_t selected,
                                   bool scanning, const char *setup_ap_ssid,
                                   bool can_cancel) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiList && s_wifi_state) {
        update_wifi_list(networks, count, selected, scanning, can_cancel);
    } else {
        build_wifi_list(networks, count, selected, scanning, setup_ap_ssid,
                        can_cancel);
        lv_screen_load(s_screen);
    }
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_wifi_keyboard(const char *ssid,
                                       std::size_t password_length,
                                       WifiKeyboardPage page,
                                       std::size_t selected,
                                       const char *status) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiKeyboard && s_wifi_password_mask) {
        update_wifi_keyboard(ssid, password_length, page, selected, status);
    } else {
        build_wifi_keyboard(ssid, password_length, page, selected, status);
        lv_screen_load(s_screen);
    }
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_wifi_connecting(const char *ssid, const char *detail) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, detail);
    lv_screen_load(s_screen);
    if (s_ui_timer) lv_timer_resume(s_ui_timer);
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_wifi_success(const char *ssid) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, "连接成功 · 确定返回", true);
    lv_screen_load(s_screen);
    if (s_ui_timer) lv_timer_pause(s_ui_timer);
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_saved_wifi(const char *const *ssids,
                                    std::size_t count, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_saved_wifi(ssids, count, selected);
    bsp_lvgl_unlock();
}

void wifi_picker_ui_show_wifi_delete_confirm(const char *ssid,
                                             std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_delete_confirm(ssid, selected);
    bsp_lvgl_unlock();
}

const char *wifi_picker_ui_wifi_key(WifiKeyboardPage page, std::size_t index) {
    return passport::wifi_keyboard::key(page, index);
}

std::size_t wifi_picker_ui_wifi_key_count(WifiKeyboardPage page) {
    return passport::wifi_keyboard::page(page).count;
}
