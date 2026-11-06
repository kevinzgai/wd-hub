// Application drawer: shared phosphor palette and pixel icons.
#include "launcher/launcher.h"
#include "apps/voice/time_sync.h"
#include "launcher/app_registry.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "ui/pixel_scene.h"
#include "app_fonts.h"
#include "esp_log.h"
#include "lvgl.h"

#include <cstdio>
#include <initializer_list>

static const char *TAG = "launcher";

namespace {

constexpr size_t kIconParts = 10;

// 旅行仪器配色:与首页(app_home.cc)完全同源,风格统一。
#define kBg (UI_BG)          // 屏底炭黑
#define kCardBg (UI_SURFACE)      // 卡片底
#define kPhos (UI_ACCENT)        // 琥珀:图标/标题/选中态
#define kPhosDim (UI_TEXT_DIM)     // 灰米色:副标题/未选中
#define kPhosBorder (UI_BORDER)  // 卡片/底板描边
#define kStatus (ui_theme_is_multicolor() ? UI_TEXT : kPhos)

lv_obj_t *s_screen;
lv_obj_t *s_card;
lv_obj_t *s_previous, *s_next;
lv_obj_t *s_app_title;
lv_obj_t *s_clock;
lv_obj_t *s_page_number;
lv_obj_t *s_icon;
lv_obj_t *s_icon_parts[kIconParts];
lv_obj_t *s_wifi_bars[3];
ui_pixel_battery_t s_battery;
size_t s_selected_index;

void draw_catalogue_stage(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    // A single ticket: notched corners and one accent tab. The page number in
    // the corner is the only position cue, so no brackets or pager dots.
    ink.box(0, 0, 216, 146, UI_SURFACE);
    for (int x : {0, 210}) {
        ink.box(x, 0, 6, 6, UI_BG); ink.box(x, 140, 6, 6, UI_BG);
    }
    ink.box(14, 12, 14, 2, UI_ACCENT);
}

// Up/down neighbours read as a wheel: a 7x4 pixel chevron before the name.
void draw_neighbour_chevron(lv_event_t *event) {
    auto *label = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const char *text = lv_label_get_text(label);
    if (!text || !text[0]) return;
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int w = lv_obj_get_width(label);
    const int x = (w - size.x) / 2 - 14;
    const bool up = lv_event_get_user_data(event) != nullptr;
    pixel_scene::Ink ink(event);
    for (int row = 0; row < 4; ++row) {
        const int half = up ? row : 3 - row;
        ink.box(x + 3 - half, 8 + row, 1 + half * 2, 1, UI_BORDER);
    }
}

const char *app_chinese_name(passport_app_id_t id) {
    // 2026-09-13 修复(D1):名称原本在 launcher 里硬编码一份、在 passport_app_t
    // 里又各写一份,两边悄悄跑偏(注册表"城市收音机" vs 抽屉"收音机",
    // "随身 AI 语音" vs "AI语音")。改成**以注册表为唯一来源**,只有没注册进
    // 注册表的内置页(配网/设置)才走兜底表。
    const passport_app_t *app = app_registry_get_by_id(id);
    if (app != nullptr && app->name != nullptr && app->name[0] != '\0') {
        return app->name;
    }
    switch (id) {
        case APP_ID_WIFI: return "无线网络";
        case APP_ID_SETTINGS: return "设置";
        default: return "";
    }
}

// 像素块:方角(至多 1px 圆角),保证所有图标是同一套"积木"语言
void set_part(size_t index, int x, int y, int w, int h, uint32_t color) {
    if (index >= kIconParts) return;
    lv_obj_t *part = s_icon_parts[index];
    lv_obj_clear_flag(part, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, 0, 0);
    lv_obj_set_style_border_width(part, 0, 0);
    lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
}

// 描边块:纸底 + 墨边,用于图标里的"空心"结构(机身/屏幕)
void set_frame(size_t index, int x, int y, int w, int h, uint32_t color,
               int border) {
    if (index >= kIconParts) return;
    lv_obj_t *part = s_icon_parts[index];
    lv_obj_clear_flag(part, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, 0, 0);
    lv_obj_set_style_border_width(part, border, 0);
    lv_obj_set_style_border_color(part, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(part, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
}

// Shared neon accents stay outside every application's 88x88 icon geometry.
void draw_icon_accents(lv_event_t *event) {
    auto *icon = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_area_t origin;
    lv_obj_get_coords(icon, &origin);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t paint;
    lv_draw_rect_dsc_init(&paint);
    paint.bg_opa = LV_OPA_COVER;
    if (ui_theme_get() == UI_THEME_NEON) {
        const lv_area_t traces[] = {
            {origin.x1 + 2, origin.y1 + 12, origin.x1 + 3, origin.y1 + 31},
            {origin.x1 + 2, origin.y1 + 7, origin.x1 + 15, origin.y1 + 8},
            {origin.x2 - 18, origin.y2 - 5, origin.x2 - 2, origin.y2 - 4},
            {origin.x2 - 5, origin.y2 - 19, origin.x2 - 4, origin.y2 - 9},
        };
        for (int i = 0; i < 4; ++i) {
            paint.bg_color = lv_color_hex(i < 2 ? UI_ACCENT : UI_SECONDARY);
            lv_draw_rect(layer, &paint, &traces[i]);
        }
    }
}

// 复古配色:各应用图标用各自的旧仪器色(赭石/橄榄/砖红/灰青/芥末…),
// 不再是统一琥珀。travel 主题下 detail/warm 由主色向纸色/墨色混合得到层次。
uint32_t retro_icon_color(passport_app_id_t id) {
    switch (id) {
        case APP_ID_XIAOZHI: return 0x6E7F5C;    // 橄榄绿
        case APP_ID_MUSE: return 0x8C5A57;       // 砖红
        case APP_ID_STOPWATCH: return 0x4F6A6B;  // 灰青
        case APP_ID_POMODORO: return 0xB5623C;   // 陶土
        case APP_ID_MORSE: return 0xA8823C;      // 芥末黄
        case APP_ID_VOCAB: return 0x7A6A4F;      // 卡其
        case APP_ID_SETTINGS: return 0x7C7568;   // 灰褐
        default: return kPhos;
    }
}

// 按百分比向目标色混合(复古低饱和的关键)。
uint32_t mix_color(uint32_t color, uint32_t toward, uint8_t pct) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const unsigned a = (color >> shift) & 255u, b = (toward >> shift) & 255u;
        out |= (uint32_t)((a * (100 - pct) + b * pct) / 100) << shift;
    }
    return out;
}

void draw_app_icon(passport_app_id_t id, uint32_t color) {
    const uint32_t detail = ui_theme_is_multicolor() ? UI_SECONDARY
                                                    : mix_color(color, 0xEEE7D6, 55);
    const uint32_t warm = ui_theme_is_multicolor() ? UI_TERTIARY
                                                   : mix_color(color, 0x181B1C, 28);
    lv_obj_invalidate(s_icon);
    for (lv_obj_t *part : s_icon_parts) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);

    switch (id) {
        case APP_ID_HOME:
            // 小房子:阶梯屋顶 + 描边屋身 + 门 + 窗
            set_part(0, 14, 30, 60, 8, color);
            set_part(1, 22, 22, 44, 8, color);
            set_part(2, 32, 14, 24, 8, color);
            set_frame(3, 20, 38, 48, 34, color, 4);
            set_part(4, 40, 50, 10, 22, color);   // 门
            set_part(5, 26, 44, 8, 8, color);     // 窗
            break;
        case APP_ID_XIAOZHI:
            // 机器人头(吉祥物同族):头 + 大眼 + 天线 + 围巾
            set_frame(0, 16, 18, 56, 44, color, 4);
            set_part(1, 28, 30, 8, 12, color);    // 左眼
            set_part(2, 52, 30, 8, 12, color);    // 右眼
            set_part(3, 38, 50, 12, 4, color);    // 嘴
            set_part(4, 42, 8, 4, 10, detail);     // 天线
            set_part(5, 38, 4, 12, 6, detail);      // 天线头
            set_part(6, 26, 64, 36, 6, warm);     // 围巾
            break;
        case APP_ID_MUSE:
                set_frame(0, 18, 18, 52, 46, color, 4);
                set_part(1, 29, 31, 6, 12, color);
                set_part(2, 53, 31, 6, 12, color);
                set_part(3, 36, 53, 16, 4, color);
                set_part(4, 6, 34, 8, 20, color);
                set_part(5, 74, 34, 8, 20, color);
                break;
        case APP_ID_WIFI:
            // 信号塔:四级阶梯(与其它图标的"方块"语言一致)
            set_part(0, 14, 56, 12, 18, color);
            set_part(1, 32, 44, 12, 30, color);
            set_part(2, 50, 30, 12, 44, color);
            set_part(3, 68, 16, 12, 58, color);
            break;
        case APP_ID_SETTINGS:
            // 滑块三行:横轨 + 错位方块钮
            set_part(0, 14, 20, 60, 6, color);
            set_part(1, 14, 41, 60, 6, detail);
            set_part(2, 14, 62, 60, 6, warm);
            set_part(3, 26, 14, 16, 18, color);
            set_part(4, 52, 35, 16, 18, detail);
            set_part(5, 22, 56, 16, 18, warm);
            break;
        case APP_ID_STOPWATCH:
            set_part(0, 34, 6, 20, 6, color);
            set_part(1, 41, 12, 6, 8, color);
            set_part(2, 66, 18, 10, 6, color);
            set_frame(3, 16, 24, 56, 50, color, 4);
            set_part(4, 41, 34, 6, 20, color);
            set_part(5, 47, 48, 14, 6, color);
            break;
        case APP_ID_POMODORO:
            // Stepped tomato outline and clock hands use the same 4px strokes.
            // Carve the two overlapping body blocks with the current page color.
            set_part(0, 42, 8, 4, 16, ui_theme_is_multicolor() ? UI_SUCCESS : color);
            set_part(1, 26, 14, 16, 4, ui_theme_is_multicolor() ? UI_SUCCESS : color);
            set_part(2, 46, 14, 16, 4, ui_theme_is_multicolor() ? UI_SUCCESS : color);
            set_part(3, 24, 24, 40, 48, color);
            set_part(4, 16, 32, 56, 32, color);
            set_part(5, 28, 28, 32, 40, UI_BG);
            set_part(6, 20, 36, 48, 24, UI_BG);
            set_part(7, 42, 34, 4, 16, color);
            set_part(8, 46, 46, 12, 4, color);
            set_part(9, 42, 60, 4, 4, color);
            break;
        case APP_ID_MORSE:
            // 摩斯电键:底板 + 立柱 + 杠杆 + 捏柄,上方两点一划示意电码
            set_frame(0, 14, 58, 60, 14, color, 3);   // 底板
            set_part(1, 30, 48, 8, 10, color);        // 立柱
            set_part(2, 22, 40, 50, 6, color);        // 杠杆
            set_part(3, 62, 28, 12, 12, color);       // 捏柄
            set_part(4, 22, 22, 6, 6, detail);        // 嘀
            set_part(5, 34, 22, 6, 6, detail);        // 嘀
            set_part(6, 46, 22, 18, 6, warm);         // 嗒
            break;
        case APP_ID_VOCAB:
            // 打开的书:左右两页 + 书脊 + 四行字(与其它图标的方块语言一致)
            set_frame(0, 10, 26, 34, 42, color, 2);    // 左页
            set_frame(1, 44, 26, 34, 42, color, 2);    // 右页
            set_part(2, 42, 22, 4, 50, detail);        // 书脊
            set_part(3, 17, 36, 20, 4, color);         // 左页行
            set_part(4, 17, 46, 20, 4, warm);
            set_part(5, 51, 36, 20, 4, color);         // 右页行
            set_part(6, 51, 46, 20, 4, warm);
            set_part(7, 17, 56, 14, 4, detail);
            set_part(8, 51, 56, 14, 4, detail);
            break;
        default:
            break;
    }
}

void update_card_content() {
    const passport_app_t *app = app_registry_get_by_index(s_selected_index);
    if (!app) return;
    lv_obj_set_style_text_font(s_app_title,
        app->id == APP_ID_POMODORO ? &pomodoro_font_20
        : app->id == APP_ID_VOCAB ? &vocab_title_20
                                  : &ui_font_20, 0);
    lv_label_set_text(s_app_title, app_chinese_name(app->id));
    lv_label_set_text_fmt(s_page_number, "%02u / %02u", (unsigned)s_selected_index + 1,
                          (unsigned)app_registry_get_count());
    uint32_t color = kPhos;
    if (ui_theme_is_multicolor()) {
        switch (app->id) {
            case APP_ID_POMODORO: color = UI_TERTIARY; break;
            case APP_ID_XIAOZHI: case APP_ID_STOPWATCH: color = UI_SUCCESS; break;
            default: break;
        }
        ui_pixel_text_color(s_app_title, color);
    } else {
        // 旅行仪器主题:图标改复古分色,标题保持琥珀以免标题忽明忽暗。
        color = retro_icon_color(app->id);
    }
    draw_app_icon(app->id, color);
    lv_obj_invalidate(s_card);

    const size_t count = app_registry_get_count();
    const auto *previous = app_registry_get_by_index((s_selected_index + count - 1) % count);
    const auto *next = app_registry_get_by_index((s_selected_index + 1) % count);
    ui_pixel_label_set_text(s_previous, previous ? app_chinese_name(previous->id) : "");
    ui_pixel_label_set_text(s_next, next ? app_chinese_name(next->id) : "");
}

// Selection follows physical UP/DOWN: only the icon settles, text stays readable.
// One 140ms animation, reset to the same resting position even on rapid repeats.
void icon_y_anim(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

void card_nudge(int dir) {
    if (!s_icon) return;
    lv_anim_delete(s_icon, icon_y_anim);
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_icon);
    lv_anim_set_exec_cb(&b, icon_y_anim);
    lv_anim_set_values(&b, 18 - 6 * dir, 18);
    lv_anim_set_duration(&b, 140);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

}  // namespace

void launcher_ui_init(void) {
    bsp_lvgl_lock(-1);

    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    // 与首页一致的绿磷光底(不再用天空/云/草地的像素风背景)
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    // ---- 状态栏 (y0..24):与首页同款 —— 炭灰条 + 共享 Wi-Fi 格 + 电池 ----
    ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(s_screen, 12, 18, s_wifi_bars);
    s_battery = ui_pixel_battery_create(s_screen, 206, 6, kStatus,
                                        &lv_font_montserrat_14);

    lv_obj_t *catalogue = ui_pixel_label(s_screen, "应用", &buddy_font_16, UI_TEXT);
    ui_pixel_status_text_align(catalogue, 42, 40, LV_TEXT_ALIGN_LEFT);
    s_clock = ui_pixel_label(s_screen, "--:--", &lv_font_montserrat_14, UI_TEXT);
    ui_pixel_status_text_align(s_clock, 92, 56, LV_TEXT_ALIGN_CENTER);
    s_previous = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    s_next = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_previous, 12, 50); lv_obj_set_size(s_previous, 216, 22);
    lv_obj_set_pos(s_next, 12, 242); lv_obj_set_size(s_next, 216, 22);
    for (lv_obj_t *peek : {s_previous, s_next}) {
        lv_obj_set_style_text_align(peek, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(peek, LV_OPA_80, 0);
        lv_label_set_long_mode(peek, LV_LABEL_LONG_DOT);
    }
    lv_obj_add_event_cb(s_previous, draw_neighbour_chevron, LV_EVENT_DRAW_MAIN_END, s_previous);
    lv_obj_add_event_cb(s_next, draw_neighbour_chevron, LV_EVENT_DRAW_MAIN_END, nullptr);
    s_card = block(s_screen, 12, 84, 216, 146, kCardBg);
    lv_obj_add_event_cb(s_card, draw_catalogue_stage, LV_EVENT_DRAW_MAIN, nullptr);
    s_page_number = ui_pixel_label(s_card, "", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_set_pos(s_page_number, 140, 6);
    lv_obj_set_size(s_page_number, 64, 18);
    lv_obj_set_style_text_align(s_page_number, LV_TEXT_ALIGN_RIGHT, 0);
    s_icon = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, 88, 88);
    lv_obj_set_pos(s_icon, 64, 18);
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_icon, draw_icon_accents, LV_EVENT_DRAW_MAIN, nullptr);
    for (lv_obj_t *&part : s_icon_parts) {
        part = lv_obj_create(s_icon);
        lv_obj_remove_style_all(part);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
    }
    s_app_title = ui_pixel_label(s_card, "", &ui_font_20, UI_TEXT);
    lv_obj_set_pos(s_app_title, 0, 105); lv_obj_set_size(s_app_title, 216, 28);
    lv_label_set_long_mode(s_app_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_app_title, LV_TEXT_ALIGN_CENTER, 0);

    ui_pixel_footer(s_screen, "上下选择 · 确定打开", "长按确定返回首页", &buddy_font_16);

    update_card_content();
    launcher_ui_update_status();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "Launcher UI initialized (travel instrument), %u apps", (unsigned)app_registry_get_count());
}

void launcher_ui_show(void) {
    if (!s_screen) {
        launcher_ui_init();
        return;
    }
    bsp_lvgl_lock(-1);
    update_card_content();
    launcher_ui_update_status();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();
}

void launcher_ui_hide(void) {
    // Launcher 不与重型 App 同时常驻。语音类 App 的音频驱动和复杂 UI 会把单核
    // C3 的内部堆推到极限，进入 App 前释放主页可避免 LVGL 分配失败崩溃。
    bsp_lvgl_lock(-1);
    if (s_screen) lv_obj_delete(s_screen);
    s_screen = nullptr;
    s_card = nullptr;
    s_previous = s_next = nullptr;
    s_app_title = nullptr;
    s_page_number = nullptr;
    s_icon = nullptr;
    s_battery = {nullptr, nullptr};
    for (lv_obj_t *&part : s_icon_parts) part = nullptr;
    for (lv_obj_t *&bar : s_wifi_bars) bar = nullptr;
    bsp_lvgl_unlock();
}

void launcher_ui_update_status(void) {
    if (!s_screen) return;
    char time[8]; time_sync_format_local(time, sizeof(time)); ui_pixel_label_set_text(s_clock, time);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    const bool connected = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t color = kPhosDim;
    if (connected) {
        const int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        color = kStatus;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, color);
}

void launcher_ui_on_key(uint8_t btn, app_btn_event_t event) {
    if (event != BTN_EVT_CLICK && event != BTN_EVT_LONG) return;
    const size_t count = app_registry_get_count();
    if (!count) return;

    if (btn == BSP_BTN_UP && event == BTN_EVT_CLICK) {
        // Previous app: a short vertical icon cue, with text kept still.
        s_selected_index = (s_selected_index + count - 1) % count;
        bsp_lvgl_lock(-1);
        update_card_content();
        card_nudge(1);
        bsp_lvgl_unlock();
    } else if (btn == BSP_BTN_DOWN && event == BTN_EVT_CLICK) {
        // Next app: reverse the same short cue.
        s_selected_index = (s_selected_index + 1) % count;
        bsp_lvgl_lock(-1);
        update_card_content();
        card_nudge(-1);
        bsp_lvgl_unlock();
    } else if (btn == BSP_BTN_OK) {
        const passport_app_t *app = app_registry_get_by_index(s_selected_index);
        if (app) {
            ESP_LOGI(TAG, "User launched: %s (ID: %d)", app->name, app->id);
            app_registry_switch_to(app->id);
        }
    }
}
