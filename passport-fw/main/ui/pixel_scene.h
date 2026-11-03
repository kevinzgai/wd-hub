#pragma once
#include "lvgl.h"

// Tiny draw vocabulary: no image decoding, retained child objects or canvas.
namespace pixel_scene {
struct Ink {
    lv_layer_t *layer;
    lv_area_t origin;
    lv_draw_rect_dsc_t paint;
    explicit Ink(lv_event_t *event) : layer(lv_event_get_layer(event)) {
        lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &origin);
        lv_draw_rect_dsc_init(&paint);
        paint.bg_opa = LV_OPA_COVER;
    }
    void box(int x, int y, int w, int h, uint32_t color) {
        paint.bg_color = lv_color_hex(color);
        lv_area_t a{origin.x1 + x, origin.y1 + y, origin.x1 + x + w - 1, origin.y1 + y + h - 1};
        lv_draw_rect(layer, &paint, &a);
    }
    void frame(int x, int y, int w, int h, uint32_t color, int t = 1) {
        box(x, y, w, t, color); box(x, y + h - t, w, t, color);
        box(x, y, t, h, color); box(x + w - t, y, t, h, color);
    }
};
} // namespace pixel_scene
