// main/apps/vocab/app_vocab.cc —— 英语四级背单词(独立应用)。
//
// 交互:上单击=上一页,下单击/OK=下一页,长按 OK=返回应用板块(全局手势)。
// 版式:每页 2 个单词,英文大字 + 词性 + 中文释义(中英对照),简单清楚。
// 词库:本地编译期常量(2000 词,离线,无网络依赖),见 cet4_vocab.h。
// 进度:每次翻页写入 NVS(vocab/page),下次进入停在上次的阅读位置。
#include "apps/app_base.h"
#include "apps/vocab/cet4_vocab.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr char kTag[] = "vocab";
constexpr uint8_t kPerPage = 2;
constexpr char kNvsNamespace[] = "vocab";
constexpr char kNvsKey[] = "page";

lv_obj_t *s_screen;
lv_obj_t *s_en[kPerPage];
lv_obj_t *s_pos[kPerPage];
lv_obj_t *s_zh[kPerPage];
lv_obj_t *s_page_label;
ui_pixel_battery_t s_battery{};

uint16_t s_page;
uint16_t s_page_count;

uint16_t page_count() { return (CET4_VOCAB_COUNT + kPerPage - 1) / kPerPage; }
uint16_t word_index(uint16_t page, uint8_t slot) { return page * kPerPage + slot; }

void load_progress() {
    s_page = 0;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, kNvsKey, &v) == ESP_OK && v >= 0) s_page = (uint16_t)v;
    nvs_close(h);
}

void save_progress() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, kNvsKey, (int32_t)s_page);
    nvs_commit(h);
    nvs_close(h);
}

// 英文统一小写显示(词表含少量首字母大写的专有名词)。LVGL 会复制文本,局部缓冲即可。
void set_en_text(lv_obj_t *label, const char *en) {
    char buf[32];
    size_t n = 0;
    for (const char *p = en; *p && n < sizeof(buf) - 1; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[n++] = c;
    }
    buf[n] = '\0';
    lv_label_set_text(label, buf);
}

void render_page() {
    if (!bsp_lvgl_lock(500)) return;
    if (s_page >= s_page_count) s_page = s_page_count ? (uint16_t)(s_page_count - 1) : 0;
    for (uint8_t i = 0; i < kPerPage; ++i) {
        const uint16_t idx = word_index(s_page, i);
        const bool valid = idx < CET4_VOCAB_COUNT;
        const cet4_word_t *w = valid ? &CET4_VOCAB[idx] : nullptr;
        if (s_en[i]) {
            if (valid) set_en_text(s_en[i], w->en);
            else lv_label_set_text(s_en[i], "");
        }
        if (s_pos[i]) lv_label_set_text(s_pos[i], valid ? w->pos : "");
        if (s_zh[i]) lv_label_set_text(s_zh[i], valid ? w->zh : "");
    }
    if (s_page_label) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%02u / %02u", (unsigned)(s_page + 1),
                      (unsigned)s_page_count);
        lv_label_set_text(s_page_label, buf);
    }
    bsp_lvgl_unlock();
}

void turn(int dir) {
    const int32_t next = (int32_t)s_page + dir;
    if (next < 0 || next >= (int32_t)s_page_count) return;
    s_page = (uint16_t)next;
    render_page();
    save_progress();
}

void create_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = ui_pixel_label(top, "背单词", &vocab_title_20, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    s_page_label = ui_pixel_label(s_screen, "", &lv_font_montserrat_14, UI_TEXT_DIM);
    lv_obj_set_style_text_align(s_page_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_page_label, 100, 18);
    lv_obj_set_pos(s_page_label, 126, 30);

    lv_obj_t *rule = lv_obj_create(s_screen);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 216, 1);
    lv_obj_set_pos(rule, 12, 46);
    lv_obj_set_style_bg_color(rule, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    // 每页两个词:上块 y50..152,下块 y158..266,各自英文/词性/释义三行。
    for (uint8_t i = 0; i < kPerPage; ++i) {
        const int base = 50 + i * 108;
        s_en[i] = ui_pixel_label(s_screen, "", &lv_font_montserrat_20, UI_TEXT);
        lv_obj_set_pos(s_en[i], 14, base);
        lv_obj_set_width(s_en[i], 212);
        lv_label_set_long_mode(s_en[i], LV_LABEL_LONG_MODE_DOTS);

        s_pos[i] = ui_pixel_label(s_screen, "", &lv_font_montserrat_14, UI_TEXT_DIM);
        lv_obj_set_pos(s_pos[i], 14, base + 28);

        s_zh[i] = ui_pixel_label(s_screen, "", &vocab_font_16, UI_TEXT);
        lv_obj_set_pos(s_zh[i], 14, base + 48);
        lv_obj_set_width(s_zh[i], 212);
        lv_label_set_long_mode(s_zh[i], LV_LABEL_LONG_MODE_WRAP);
    }

    lv_obj_t *mid = lv_obj_create(s_screen);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, 216, 1);
    lv_obj_set_pos(mid, 12, 156);
    lv_obj_set_style_bg_color(mid, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);

    ui_pixel_footer(s_screen, "上:上一页 · 下/OK:下一页", "长按 OK 返回", &buddy_font_16);
}

void start() {
    load_progress();
    s_page_count = page_count();
    bsp_lvgl_lock(-1);
    if (!s_screen) create_ui();
    render_page();
    lv_screen_load(s_screen);
    bsp_lvgl_unlock();
    ESP_LOGI(kTag, "opened at page %u/%u (words=%u)", (unsigned)(s_page + 1),
             (unsigned)s_page_count, (unsigned)CET4_VOCAB_COUNT);
}

void stop() {
    save_progress();
    bsp_lvgl_lock(-1);
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    for (uint8_t i = 0; i < kPerPage; ++i) {
        s_en[i] = s_pos[i] = s_zh[i] = nullptr;
    }
    s_page_label = nullptr;
    s_battery = {};
    bsp_lvgl_unlock();
}

void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK) return;   // 长按 OK 走全局返回
    if (btn == BSP_BTN_UP) {
        turn(-1);
    } else if (btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) {
        turn(1);
    }
}

}  // namespace

extern const passport_app_t g_vocab_app = {
    .id = APP_ID_VOCAB,
    .name = "背单词",
    .en_name = "CET-4",
    .desc = "CET-4 词库\n每页两个词",
    .tag = "OFFLINE",
    .theme_color = 0x7A6A4F,
    .init = nullptr,
    .start = start,
    .stop = stop,
    .on_key = on_key,
};
