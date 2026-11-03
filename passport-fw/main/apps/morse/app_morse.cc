// main/apps/morse/app_morse.cc —— 摩斯电键(KEYER)练习页。
//
// 交互:
//   OK 按住/抬起 = 电键(发声 + 亮灯,抬键判嘀/嗒);
//   上单击 = WPM+;  上长按 = 退格删上一字符;
//   下单击 = WPM-;  下长按 = 退出返回应用板块;
//   长按 OK 归本应用自己处理(hold_ok=true),registry 不做"返回":
//   电键按住不放就是正常发报动作,绝不能被当成退出手势。
// 解码:静默轮询挂在 30ms lv_timer 上(LVGL 任务上下文),按压/抬键在输入任务
// 上下文,两路共享 s_dec,用一把静态互斥量串行化(持锁微秒级,不争 LVGL 锁)。
// 音频:进入即开侧音任务(bsp_audio_init_playback,TX-only);prepare_stop 收
// morse_tone_end(含 bsp_audio_deinit)。音频不可用时仍可纯视觉练习并明示。
#include "apps/app_base.h"
#include "apps/morse/morse_code.h"
#include "apps/morse/morse_decode.h"
#include "apps/morse/morse_tone.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "morse";
constexpr uint8_t kWpmMin = 5;
constexpr uint8_t kWpmMax = 30;
constexpr uint8_t kWpmDefault = 10;
constexpr size_t kTextCap = 97;      // 上屏文本环形容量;满时丢前 32 字符

lv_obj_t *s_screen;
lv_obj_t *s_lamp;
lv_obj_t *s_wpm_num;
lv_obj_t *s_note;
lv_obj_t *s_elem;
lv_obj_t *s_panel;
lv_obj_t *s_text_label;
lv_timer_t *s_poll_timer;
ui_pixel_battery_t s_battery{};
uint8_t s_battery_div = 0;

morse_decoder_t s_dec;
uint8_t s_wpm = kWpmDefault;
bool s_audio_ok;
bool s_key_down;
int64_t s_press_us;
int64_t s_release_us;
char s_text[kTextCap];
SemaphoreHandle_t s_dec_mutex;
StaticSemaphore_t s_dec_mutex_buf;

void dec_lock() { xSemaphoreTake(s_dec_mutex, portMAX_DELAY); }
void dec_unlock() { xSemaphoreGive(s_dec_mutex); }

// ---- UI 助手:按需自持 LVGL 锁(按键回调在输入任务,不在 LVGL 任务) ----

void lamp_set(bool on) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_lamp) {
        ui_pixel_bg_color(s_lamp, on ? UI_AMBER : UI_PAPER);
        lv_obj_set_style_border_color(s_lamp, lv_color_hex(on ? UI_AMBER : UI_BORDER), 0);
    }
    bsp_lvgl_unlock();
}

void wpm_refresh() {
    if (!bsp_lvgl_lock(500)) return;
    if (s_wpm_num) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(s_wpm));
        lv_label_set_text(s_wpm_num, buf);
    }
    bsp_lvgl_unlock();
}

// 当前字符已积累的嘀/嗒序列,给练习者即时反馈。
void elem_refresh() {
    char buf[8];
    dec_lock();
    uint8_t n = s_dec.len;
    const uint8_t bits = s_dec.bits;
    dec_unlock();
    if (n > 7) n = 7;
    for (uint8_t i = 0; i < n; i++) buf[i] = (bits & (uint8_t)(1u << i)) ? '-' : '.';
    buf[n] = '\0';
    if (!bsp_lvgl_lock(500)) return;
    if (s_elem) lv_label_set_text(s_elem, buf);
    bsp_lvgl_unlock();
}

// 退格:删除 s_text 最后一个字符;同时清掉解码器里还没产出的半个字符,
// 避免退格后残留元素被误判成新字符。
void backspace() {
    dec_lock();
    s_dec.len = 0;
    s_dec.bits = 0;
    s_dec.char_done = false;
    s_dec.word_done = false;
    dec_unlock();

    // 空格(词间隔)也算一个字符,一次退格只删一个字符。
    const size_t len = std::strlen(s_text);
    if (len > 0) s_text[len - 1] = '\0';

    if (!bsp_lvgl_lock(500)) return;
    if (s_text_label && s_panel) {
        lv_label_set_text(s_text_label, s_text);
        lv_obj_scroll_to_y(s_panel, LV_COORD_MAX, LV_ANIM_OFF);
    }
    if (s_elem) lv_label_set_text(s_elem, "");
    bsp_lvgl_unlock();
}

// 解码产出字符后追加到文本。
void text_append(char c) {
    size_t len = std::strlen(s_text);
    if (len + 2 > kTextCap) {
        std::memmove(s_text, s_text + 32, kTextCap - 32);   // 连同结尾 '\0' 一起搬
        len = std::strlen(s_text);
    }
    s_text[len] = c;
    s_text[len + 1] = '\0';

    if (!bsp_lvgl_lock(500)) return;
    if (s_text_label && s_panel) {
        lv_label_set_text(s_text_label, s_text);
        lv_obj_scroll_to_y(s_panel, LV_COORD_MAX, LV_ANIM_OFF);
    }
    bsp_lvgl_unlock();
}

// 静默轮询(LVGL 任务上下文,控件直写)。字符/词间隔都由这里产出。
void poll_tick(lv_timer_t *) {
    if (!s_screen) return;
    if (!s_key_down) {
        const int64_t silent_us = esp_timer_get_time() - s_release_us;
        const uint32_t silent_ms = (uint32_t)(silent_us / 1000);
        dec_lock();
        const char c = morse_decoder_poll(&s_dec,
            silent_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)silent_ms);
        dec_unlock();
        if (c != '\0') {
            text_append(c);
            // 解码产出后 bits/len 已清零,元素行同步清空
            if (!bsp_lvgl_lock(500)) return;
            if (s_elem) lv_label_set_text(s_elem, "");
            bsp_lvgl_unlock();
        }
    }
    if (++s_battery_div >= 167) {   // ≈5s
        s_battery_div = 0;
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    }
}

void create_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = ui_pixel_label(top, "摩斯电键", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    // ---- 电键行 (y34..92):方灯 + WPM 大读数 + 当前元素序列 ----
    s_lamp = lv_obj_create(s_screen);
    lv_obj_set_size(s_lamp, 56, 56);
    lv_obj_set_pos(s_lamp, 16, 38);
    lv_obj_set_style_radius(s_lamp, 6, 0);
    lv_obj_set_style_border_width(s_lamp, 2, 0);
    lv_obj_set_style_border_color(s_lamp, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_color(s_lamp, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_bg_opa(s_lamp, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_lamp, LV_OBJ_FLAG_SCROLLABLE);

    s_wpm_num = ui_pixel_label(s_screen, "10", &lv_font_montserrat_32, UI_TEXT);
    lv_obj_set_pos(s_wpm_num, 84, 32);
    lv_obj_t *unit = ui_pixel_label(s_screen, "WPM", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_set_pos(unit, 86, 72);

    s_elem = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_elem, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_elem, lv_color_hex(UI_AMBER), 0);
    lv_obj_set_style_text_align(s_elem, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_elem, 126, 24);
    lv_obj_set_pos(s_elem, 102, 36);
    lv_label_set_text(s_elem, "");

    s_note = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_note, 126, 18);
    lv_obj_set_pos(s_note, 102, 66);

    // ---- 分隔线 + 上屏文本面板 (y100..266) ----
    lv_obj_t *rule = lv_obj_create(s_screen);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 216, 1);
    lv_obj_set_pos(rule, 12, 100);
    lv_obj_set_style_bg_color(rule, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    s_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_panel, 216, 160);
    lv_obj_set_pos(s_panel, 12, 106);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);

    s_text_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_text_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_text_label, lv_color_hex(UI_TEXT), 0);
    lv_label_set_long_mode(s_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text_label, 196);
    lv_label_set_text(s_text_label, "");

    ui_pixel_footer(s_screen, "OK键发报 · 上下调 WPM", "上长按退格 · 下长按退出",
                    &buddy_font_16);
}

void init() {
    if (!s_dec_mutex) s_dec_mutex = xSemaphoreCreateMutexStatic(&s_dec_mutex_buf);
}

void start() {
    // 音频初始化不持 LVGL 锁(I2C/codec 配置可达数十毫秒)。
    s_audio_ok = morse_tone_begin() == ESP_OK;
    if (!s_audio_ok) ESP_LOGW(kTag, "sidetone unavailable; visual-only practice");

    bsp_lvgl_lock(-1);
    if (!s_screen) create_ui();
    dec_lock();
    morse_decoder_init(&s_dec, s_wpm);
    dec_unlock();
    s_text[0] = '\0';
    s_key_down = false;
    s_release_us = esp_timer_get_time();
    s_battery_div = 0;
    wpm_refresh();
    elem_refresh();
    if (s_text_label) lv_label_set_text(s_text_label, "");
    if (s_note) lv_label_set_text(s_note, s_audio_ok ? "" : "音频不可用");
    lv_screen_load(s_screen);
    if (!s_poll_timer) s_poll_timer = lv_timer_create(poll_tick, 30, nullptr);
    bsp_lvgl_unlock();
    ESP_LOGI(kTag, "keyer started; audio=%d wpm=%u", (int)s_audio_ok, (unsigned)s_wpm);
}

bool prepare_stop() {
    s_key_down = false;
    return morse_tone_end() == ESP_OK;   // 超时返回 false,registry 会重试
}

void stop() {
    bsp_lvgl_lock(-1);
    if (s_poll_timer) { lv_timer_delete(s_poll_timer); s_poll_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_lamp = s_wpm_num = s_note = s_elem = nullptr;
    s_panel = s_text_label = nullptr;
    s_battery = {};
    bsp_lvgl_unlock();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (btn == BSP_BTN_OK) {
        if (event == BTN_EVT_PRESS) {
            s_key_down = true;
            s_press_us = esp_timer_get_time();
            if (s_audio_ok) morse_tone_gate(true);
            lamp_set(true);
        } else if (event == BTN_EVT_RELEASE) {
            if (!s_key_down) return;   // 被唤醒护栏吞掉的按下,忽略对应抬起
            s_key_down = false;
            s_release_us = esp_timer_get_time();
            if (s_audio_ok) morse_tone_gate(false);
            lamp_set(false);
            uint32_t ms = (uint32_t)((s_release_us - s_press_us) / 1000);
            if (ms > UINT16_MAX) ms = UINT16_MAX;
            dec_lock();
            morse_decoder_mark(&s_dec, (uint16_t)ms);
            dec_unlock();
            elem_refresh();
        }
        // CLICK/LONG 一律忽略:OK 是电键,按住不放是正常发报动作;
        // 长按 OK 的"返回"由 app_registry 全局处理。
        return;
    }

    if (btn == BSP_BTN_UP) {
        if (event == BTN_EVT_CLICK) {
            if (s_wpm < kWpmMax) s_wpm++;
            dec_lock();
            morse_decoder_set_wpm(&s_dec, s_wpm);   // 丢弃积累的半个字符
            dec_unlock();
            elem_refresh();
            wpm_refresh();
        } else if (event == BTN_EVT_LONG) {
            // 长按上键:退格删上一字符。
            backspace();
        }
        return;
    }

    if (btn == BSP_BTN_DOWN) {
        if (event == BTN_EVT_CLICK) {
            if (s_wpm > kWpmMin) s_wpm--;
            dec_lock();
            morse_decoder_set_wpm(&s_dec, s_wpm);
            dec_unlock();
            elem_refresh();
            wpm_refresh();
        } else if (event == BTN_EVT_LONG) {
            // 长按下键:退出返回应用板块。
            app_registry_switch_to(APP_ID_LAUNCHER);
        }
        return;
    }
}

}  // namespace

extern const passport_app_t g_morse_app = {
    .id = APP_ID_MORSE,
    .name = "Morse Keyer",   // 启动器卡片用英文名:ui_font_20 未覆盖「摩」字
    .en_name = "MORSE",
    .desc = "按住OK发报\n上下调 WPM",
    .tag = "CW",
    .theme_color = 0xDDB36C,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
    .hold_ok = true,   // 长按 OK 是"按住发报",不是返回手势
};
