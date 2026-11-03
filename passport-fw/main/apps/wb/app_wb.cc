// passport-fw/main/apps/wb/app_wb.cc —— 应用外壳：只做“按键 → 业务动作”、
// “1s 时钟 → 快照/待机/UI 同步”。不包含任何布局常量。
#include "app_wb.h"

#include <atomic>
#include <cstdio>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "launcher/app_registry.h"
#include "wb/wb_biz.h"
#include "wb/wb_link.h"
#include "wb/wb_power.h"
#include "wb/wb_ui.h"

namespace {

const char *TAG = "app_wb";

std::atomic<bool> s_dirty{true};
std::atomic<bool> s_running{false};
bool s_swallow_until_release = false;
uint64_t s_last_battery_us = 0;
lv_timer_t *s_tick_timer = nullptr;

void request_redraw() { s_dirty.store(true, std::memory_order_release); }

// wb_task 上下文：只置脏标记 + 处理唤醒；绝不直接碰 LVGL。
void on_biz_change()
{
    request_redraw();
    const unsigned wake = wb_biz_take_wake();
    if (wake) wb_power_note_activity();
}

// 由 wb_power 的 1s tick（LVGL 上下文）调用。
void on_power_ui(bool standby)
{
    (void)standby;
    request_redraw();
}

void battery_maybe_report(bool connected)
{
    const uint64_t now = esp_timer_get_time();
    if (!connected || now - s_last_battery_us < 30000000ULL) return;
    s_last_battery_us = now;
    const int soc = bsp_battery_soc();
    if (soc < 0) return;
    if (wb_biz_send_battery(soc)) ESP_LOGI(TAG, "上报电量 %d%%", soc);
}

void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_running.load(std::memory_order_acquire)) return;

    bool connected = false;
    if (wb_link_take_conn_change(&connected)) {
        wb_biz_set_connected(connected);
        ESP_LOGI(TAG, "BLE %s", connected ? "已连接" : "已断开");
    }
    if (wb_link_take_hello()) {
        if (wb_biz_send_hello()) ESP_LOGI(TAG, "已发送 hello");
    }
    wb_link_poll();

    wb_view_t view;
    wb_biz_snapshot(&view);
    wb_power_set_busy(view.busy);
    wb_power_tick();

    if (s_dirty.exchange(false, std::memory_order_acq_rel))
        wb_ui_sync(&view, wb_power_is_standby());

    battery_maybe_report(connected);
}

void wb_init()
{
    if (!wb_biz_start(on_biz_change))
        ESP_LOGE(TAG, "WB 业务层启动失败（仍进入占位界面）");
    wb_power_init(on_power_ui);
}

void wb_start()
{
    // 交给我们自己管理 180s 待机，禁用底座 60s 熄屏。
    app_registry_set_screen_timeout(0);
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "无法取得 LVGL 锁，界面未启动");
        return;
    }
    if (s_running.exchange(true)) {
        bsp_lvgl_unlock();
        return;
    }
    // 本机没有设置页：主亮度固定 100%，待机 50% 才是字面意义的 50%。
    bsp_display_set_master_brightness(100);
    bsp_display_backlight(100);

    wb_ui_init();
    wb_view_t view;
    wb_biz_snapshot(&view);
    wb_ui_sync(&view, wb_power_is_standby());
    s_tick_timer = lv_timer_create(tick_cb, 1000, nullptr);
    bsp_lvgl_unlock();
    request_redraw();
    ESP_LOGI(TAG, "WB 应用已启动");
}

void wb_stop()
{
    s_running.store(false, std::memory_order_release);
    // 若正处待机，退出时恢复亮度和 BLE 正常档，避免把待机状态泄漏给下一个应用。
    if (wb_power_is_standby()) wb_power_wake();
    if (!bsp_lvgl_lock(1000)) return;
    wb_power_tick();   // 恢复渲染周期（待机时被减半）
    if (s_tick_timer) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    wb_ui_deinit();
    bsp_lvgl_unlock();
    app_registry_set_screen_timeout(60);
}

bool wb_prepare_stop() { return true; }

void wb_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv)
{
    (void)mv;

    // 待机时任意键先唤醒；本次按压的后续事件全部吞掉，避免“唤醒 + 误操作”。
    if (!s_swallow_until_release && wb_power_is_standby() && event == BTN_EVT_PRESS) {
        wb_power_wake();
        s_swallow_until_release = true;
        return;
    }
    if (s_swallow_until_release) {
        if (event == BTN_EVT_RELEASE || event == BTN_EVT_LONG_UP)
            s_swallow_until_release = false;
        return;
    }

    if (event == BTN_EVT_PRESS) {
        wb_power_note_activity();
        return;   // 只用于唤醒/续期，不产生业务动作
    }

    if (event == BTN_EVT_CLICK) {
        wb_power_note_activity();
        switch (btn) {
        case BSP_BTN_UP: wb_biz_move(-1); break;
        case BSP_BTN_DOWN: wb_biz_move(1); break;
        case BSP_BTN_OK: wb_biz_confirm(); break;
        default: break;
        }
        return;
    }

    if (event == BTN_EVT_LONG) {
        wb_power_note_activity();
        if (btn == BSP_BTN_DOWN) {
            wb_biz_back();
        } else if (btn == BSP_BTN_OK) {
            // 长按 OK 保留底座“2s 提示 / 5s 关机”语义；
            // 这里只吃掉 800ms 的 LONG，避免 registry 把长 OK 当“返回首页”。
        }
    }
}

}  // namespace

extern const passport_app_t g_wb_app = {
    .id = APP_ID_WB,
    .name = "WorkBuddy",
    .en_name = "WB",
    .desc = "BLE 会话看板 · 审批 · 提问 · 待机时钟",
    .tag = "BLE",
    .theme_color = 0x6FD3FF,
    .init = wb_init,
    .start = wb_start,
    .stop = wb_stop,
    .on_key = wb_on_key,
    .prepare_stop = wb_prepare_stop,
    .hold_ok = true,
};
