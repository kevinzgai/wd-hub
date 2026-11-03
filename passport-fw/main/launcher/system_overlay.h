// System-wide notices drawn on LVGL's top layer, above whichever app is in
// front: pocket-lock hints, low battery and power-off messages.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Show text in a centered card. ms == 0 keeps it until hidden or replaced.
// Takes the (recursive) LVGL lock itself; callers may already hold it.
void system_overlay_show(const char *text, uint32_t ms);
void system_overlay_hide(void);

#ifdef __cplusplus
}
#endif
