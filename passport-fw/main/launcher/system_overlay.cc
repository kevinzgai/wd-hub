#include "launcher/system_overlay.h"

#include "app_fonts.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "ui_pixel.h"

namespace {

lv_obj_t *s_card;
lv_obj_t *s_label;
lv_timer_t *s_timer;

void delete_card() {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_card) { lv_obj_delete(s_card); s_card = nullptr; s_label = nullptr; }
}

}  // namespace

void system_overlay_show(const char *text, uint32_t ms) {
    if (!bsp_lvgl_lock(500)) return;
    if (!s_card) {
        // Created on demand and deleted when hidden: the C3 heap has no room
        // for a permanent layer that is shown a few times a day.
        s_card = lv_obj_create(lv_layer_top());
        lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_card, 200, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(s_card, 0, 0);
        lv_obj_set_style_bg_color(s_card, lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_card, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_border_width(s_card, 2, 0);
        lv_obj_set_style_pad_all(s_card, 12, 0);
        lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 0);
        s_label = ui_pixel_label(s_card, "", &buddy_font_16, UI_TEXT);
        lv_obj_set_width(s_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_label, 6, 0);
    }
    lv_label_set_text(s_label, text);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (ms) {
        s_timer = lv_timer_create([](lv_timer_t *) { delete_card(); }, ms, nullptr);
        lv_timer_set_repeat_count(s_timer, 1);
    }
    bsp_lvgl_unlock();
}

void system_overlay_hide(void) {
    if (!bsp_lvgl_lock(500)) return;
    delete_card();
    bsp_lvgl_unlock();
}

extern "C" void passport_internet_notice(bool captive) {
    system_overlay_show(captive ? "该网络需网页登录" : "该网络无法上网", 5000);
}
