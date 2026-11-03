// Power off, low battery and wake from power off.
//
// There is no power switch: "off" is deep sleep. All three keys share GPIO0,
// and only UP (0 Ω to ground) pulls it below the digital low threshold, so
// UP is the only key that can wake the chip.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// First call in app_main. After a key wake, UP must stay held ~0.8 s or the
// device goes straight back to sleep (a bump in a pocket must not boot it).
void power_boot_check(void);

// Show message (NULL: none), stop Wi-Fi and the panel, wait for UP to be released and
// enter deep sleep. Never returns.
void power_off(const char *message) __attribute__((noreturn));

// After each battery poll: warns once at 10 % (wakes the screen, dims it to
// 60 %). Returns true when the battery is empty; the caller then powers off
// from a task with a large enough stack (power_hold(3)).
bool power_battery_tick(void);

// Long OK hold from the button task: 0 released, 1 keep holding, 2 power off;
// 3 is the empty-battery power off.
void power_hold(uint8_t stage);

#ifdef __cplusplus
}
#endif
