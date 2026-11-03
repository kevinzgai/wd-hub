// passport-fw/main/wb/wb_power.c —— 180s 待机、50% 背光、LVGL 帧率减半。
//
// 与底座原本的“60s 熄屏”不叠加：WB 应用启动时把 app_registry 的熄屏超时
// 设为 0，屏幕常亮，由本模块把亮度降到 50% 并切到待机钟面。
//
// 线程约定：
//   * wb_power_note_activity() 可能来自 BLE 任务，只允许改背光 / BLE 档位 /
//     标志位，绝不直接操作 LVGL；
//   * LVGL 相关的动作（渲染周期、页面回调）全部收敛到 wb_power_tick()，
//     它由应用 1s tick 在 LVGL 上下文里调用。
#include "wb_power.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "portmacro.h"

#include "bsp_display.h"
#include "wb_link.h"

static const char *TAG = "wb_power";

static wb_power_ui_cb_t s_ui_cb;
static volatile bool s_standby;
static volatile bool s_busy;
static volatile bool s_ui_wake_pending;
static volatile bool s_refr_restore_pending;
static volatile bool s_inited;

// 秒级时间戳，32 位无符号相减可自然跨过 49 天回绕。
static volatile uint32_t s_idle_since_s;
static portMUX_TYPE s_idle_mux = portMUX_INITIALIZER_UNLOCKED;

static lv_timer_t *s_refr_timer;
static uint32_t s_refr_period_normal;
static bool s_refr_slowed;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static uint32_t idle_since_s(void)
{
    portENTER_CRITICAL(&s_idle_mux);
    const uint32_t value = s_idle_since_s;
    portEXIT_CRITICAL(&s_idle_mux);
    return value;
}

static void idle_reset(void)
{
    portENTER_CRITICAL(&s_idle_mux);
    s_idle_since_s = now_s();
    portEXIT_CRITICAL(&s_idle_mux);
}

// 只在 LVGL 上下文调用。
static void set_refresh_slowed(bool slow)
{
    if (!s_refr_timer) {
        lv_display_t *disp = lv_display_get_default();
        if (!disp) return;
        s_refr_timer = lv_display_get_refr_timer(disp);
        if (!s_refr_timer) return;
    }

    if (slow && !s_refr_slowed) {
        // LVGL 9 没有 lv_timer_get_period();刷新定时器周期来自 lv_conf。
        s_refr_period_normal = LV_DEF_REFR_PERIOD;
        if (s_refr_period_normal == 0) s_refr_period_normal = 33;
        lv_timer_set_period(s_refr_timer, s_refr_period_normal * 2);
        s_refr_slowed = true;
        ESP_LOGI(TAG, "渲染周期 %" PRIu32 "ms → %" PRIu32 "ms（帧率减半）",
                 s_refr_period_normal, s_refr_period_normal * 2);
    } else if (!slow && s_refr_slowed) {
        lv_timer_set_period(s_refr_timer, s_refr_period_normal);
        s_refr_slowed = false;
        ESP_LOGI(TAG, "渲染周期恢复 %" PRIu32 "ms", s_refr_period_normal);
    }
}

// 背光与 BLE 档位是线程安全的，可以在 BLE 任务里直接切换。
static void apply_power_state(bool on)
{
    s_standby = on;
    bsp_display_backlight(on ? 50 : 100);
    wb_link_set_power_mode(on);
    ESP_LOGI(TAG, "%s：背光 %d%%，BLE %s",
             on ? "进入待机" : "退出待机", on ? 50 : 100, on ? "省电档" : "正常档");
}

void wb_power_init(wb_power_ui_cb_t on_ui_mode)
{
    s_ui_cb = on_ui_mode;
    s_standby = false;
    s_busy = false;
    s_ui_wake_pending = false;
    s_refr_restore_pending = false;
    idle_reset();
    s_inited = true;
}

void wb_power_note_activity(void)
{
    if (!s_inited) return;
    idle_reset();
    if (s_standby) {
        apply_power_state(false);
        // 页面切换与渲染周期恢复交给下一个 tick，避免跨线程操作 LVGL。
        s_ui_wake_pending = true;
        s_refr_restore_pending = true;
    }
}

void wb_power_set_busy(bool busy)
{
    if (!s_inited) return;
    const bool was = s_busy;
    s_busy = busy;
    if (busy) {
        idle_reset();
        if (s_standby) wb_power_note_activity();
    } else if (was) {
        // 忙完重新给满 180s，避免刚结束就立刻待机。
        idle_reset();
    }
}

bool wb_power_is_standby(void) { return s_standby; }

void wb_power_wake(void)
{
    wb_power_note_activity();
}

void wb_power_tick(void)
{
    if (!s_inited) return;

    if (s_refr_restore_pending) {
        s_refr_restore_pending = false;
        set_refresh_slowed(false);
    }

    if (s_ui_wake_pending) {
        s_ui_wake_pending = false;
        if (s_ui_cb) s_ui_cb(false);
    }

    if (!s_standby && !s_busy) {
        const uint32_t idle_s = now_s() - idle_since_s();
        if (idle_s >= WB_STANDBY_IDLE_SEC) {
            apply_power_state(true);
            set_refresh_slowed(true);
            if (s_ui_cb) s_ui_cb(true);
        }
    }
}
