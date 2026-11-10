// passport-fw/main/apps/wb/app_wb.cc —— 应用外壳：只做“按键 → 业务动作”、
// “1s 时钟 → 快照/待机/UI 同步”。不包含任何布局常量。
#include "app_wb.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "launcher/app_registry.h"
#include "launcher/system_overlay.h"
#include "wb/wb_audio.h"
#include "wb/wb_biz.h"
#include "wb/wb_link.h"
#include "wb/wb_power.h"
#include "wb/wb_ui.h"
#include "wb/wb_vokie_mode.h"
#include "wb/vokie_svc.h"

namespace {

const char *TAG = "app_wb";

std::atomic<bool> s_dirty{true};
std::atomic<bool> s_running{false};
bool s_swallow_until_release = false;
uint64_t s_last_battery_us = 0;
lv_timer_t *s_tick_timer = nullptr;
int s_prev_running = -1;

// Vokie 模式浮层状态：状态回调来自 BLE/音频任务，tick（LVGL 上下文）统一渲染。
std::atomic<int> s_vokie_sts{(int)VOKIE_STS_DISCONNECTED};
bool s_vokie_ui_on = false;
int s_vokie_ui_sts = -1;

void request_redraw() { s_dirty.store(true, std::memory_order_release); }

int running_count(const wb_view_t &v)
{
    int n = 0;
    for (int i = 0; i < v.n_sess; ++i)
        if (v.sess[i].state[0] && std::strcmp(v.sess[i].state, "RUNNING") == 0) ++n;
    return n;
}

// wb_task 上下文：只置脏标记 + 处理唤醒/提示音；绝不直接碰 LVGL。
void on_biz_change()
{
    request_redraw();
    const unsigned wake = wb_biz_take_wake();
    if (wake) wb_power_note_activity();
    // 只有真正退出待机才放开音频；否则状态帧在待机中会把挂起的低电提示放出来。
    wb_audio_set_standby(wb_power_is_standby());

    wb_view_t view;
    wb_biz_snapshot(&view);
    wb_audio_set_host_mute(view.beep_vol <= 0);

    const int running = running_count(view);
    const bool finished = (s_prev_running > running);
    s_prev_running = running;

    if (wb_biz_take_test_beep()) {
        wb_audio_play(WB_SFX_CONNECTED);
    } else if (wake & (WB_WAKE_APPROVAL | WB_WAKE_ASK)) {
        // 与 S3 一致：审批/提问到达 = ATTENTION；提问优先于审批时只响一次。
        wb_audio_play(WB_SFX_ATTENTION);
    } else if (wake & WB_WAKE_TASK) {
        // S3 对新任务只唤醒不响；这里按用户要求补一个短高音（冲突已在报告说明）。
        wb_audio_play(WB_SFX_CONNECTED);
    } else if (finished) {
        // RUNNING -> 非 RUNNING：与 S3 的 DONE 提示保持一致。
        wb_audio_play(WB_SFX_DONE);
    }
}

// 由 wb_power 的 1s tick（LVGL 上下文）调用。
void on_power_ui(bool standby)
{
    wb_audio_set_standby(standby);
    request_redraw();
}

// Vokie 状态回调（BLE/音频任务上下文）：只置状态 + 刷活动时间。
void on_vokie_status(vokie_status_t st)
{
    s_vokie_sts.store((int)st, std::memory_order_release);
    wb_vokie_mode_note_activity();
    request_redraw();
}

// Vokie 模式浮层文案。system_overlay_show 自带递归 LVGL 锁。
void vokie_overlay_sync()
{
    const int st = s_vokie_sts.load(std::memory_order_acquire);
    const char *text;
    switch (st) {
    case VOKIE_STS_RECORDING:    text = "● 录音中\n点 UP 结束"; break;
    case VOKIE_STS_PROCESSING:   text = "识别处理中..."; break;
    case VOKIE_STS_SUCCESS:      text = "已发送"; break;
    case VOKIE_STS_ERROR:        text = "出错了，请重试"; break;
    case VOKIE_STS_DISCONNECTED: text = "Vokie 已断开"; break;
    default:                     text = "Vokie 就绪\n点 UP 说话\nOK 删字 / DOWN 回车"; break;
    }
    system_overlay_show(text, 0);
    s_vokie_ui_sts = st;
}

void battery_maybe_report(bool connected)
{
    const uint64_t now = esp_timer_get_time();
    if (now - s_last_battery_us < 30000000ULL) return;
    s_last_battery_us = now;
    const int soc = bsp_battery_soc();
    if (soc < 0) return;
    // 低电覆盖页、低电提示音与协议上报共用同一次读表；未连接时也喂给 UI。
    wb_ui_set_battery(soc);
    wb_ui_set_battery_mv(bsp_battery_mv());   // 低电页要用电压解释读数滞后
    wb_audio_low_battery(soc);
    request_redraw();
    if (connected && wb_biz_send_battery(soc)) ESP_LOGI(TAG, "上报电量 %d%%", soc);
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
        // hello 后立刻声明语音投递目标（直打微信），主机在第一次录音前就知道。
        wb_biz_send_voice_target();
    }
    wb_link_poll();

    wb_view_t view;
    wb_biz_snapshot(&view);
    wb_power_set_busy(view.busy);
    wb_power_tick();

    // 菜单/声音页 30s 超时与菜单「待机」请求在这里落地（LVGL 上下文）。
    if (wb_ui_tick()) s_dirty.store(true, std::memory_order_release);

    // Vokie 模式：先查 60s 无操作自动退；再处理切换沿渲染/收浮层；
    // 活动中随状态刷新文案。
    wb_vokie_mode_poll_expired();
    bool mode_now = false;
    if (wb_vokie_mode_take_changed(&mode_now)) {
        s_vokie_ui_on = mode_now;
        s_vokie_ui_sts = -1;
        if (mode_now) vokie_overlay_sync();
        else system_overlay_hide();
        request_redraw();   // 退出时恢复看板画面
    } else if (s_vokie_ui_on &&
               s_vokie_ui_sts != s_vokie_sts.load(std::memory_order_acquire)) {
        vokie_overlay_sync();
    }

    wb_audio_set_standby(wb_power_is_standby());
    wb_audio_set_host_mute(view.beep_vol <= 0);

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
    if (!wb_audio_init()) ESP_LOGW(TAG, "提示音不可用（业务层继续运行）");
    wb_audio_log_stats();
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
    vokie_svc_set_status_cb(on_vokie_status);
    const int soc = bsp_battery_soc();
    if (soc >= 0) {
        wb_ui_set_battery(soc);
        wb_ui_set_battery_mv(bsp_battery_mv());
        wb_audio_low_battery(soc);
    }
    wb_view_t view;
    wb_biz_snapshot(&view);
    s_prev_running = running_count(view);
    wb_audio_set_host_mute(view.beep_vol <= 0);
    wb_ui_sync(&view, wb_power_is_standby());
    s_tick_timer = lv_timer_create(tick_cb, 1000, nullptr);
    bsp_lvgl_unlock();
    request_redraw();
    ESP_LOGI(TAG, "WB 应用已启动");
}

void wb_stop()
{
    s_running.store(false, std::memory_order_release);
    // Vokie 模式先退出（录音中会正常收尾会话）。
    wb_vokie_mode_exit();
    system_overlay_hide();
    // 先收音频任务（最多等一段 400ms 的写完成），再拆界面。
    wb_audio_deinit();
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

// 按键短反馈：连按时限流，避免 KEY 在队列里叠成一串。
void beep_key()
{
    static uint64_t last_us = 0;
    const uint64_t now = esp_timer_get_time();
    if (now - last_us < 120000) return;
    last_us = now;
    wb_audio_play(WB_SFX_KEY);
}

wb_ui_key_t ui_key_of(uint8_t btn)
{
    return btn == BSP_BTN_UP ? WB_UI_KEY_UP
         : btn == BSP_BTN_DOWN ? WB_UI_KEY_DOWN
                               : WB_UI_KEY_OK;
}

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

    // ---- Vokie 模式：按键全部交给 vokie 语义，浮层已在 tick 渲染 ----
    // 长按 OK ≥2s 的「回看板 / 关机」手势由底座 power 流程处理（power.cc），
    // 这里只负责 800ms LONG（录音中取消）和短按。
    if (wb_vokie_mode_active()) {
        if (event == BTN_EVT_PRESS) {
            wb_power_note_activity();
            wb_vokie_mode_note_activity();
            return;
        }
        if (event == BTN_EVT_CLICK) {
            wb_power_note_activity();
            wb_vokie_mode_note_activity();
            if (!vokie_svc_is_recording()) beep_key();   // 录音中 codec 被采集占用
            vokie_svc_key(btn, false);
            return;
        }
        if (event == BTN_EVT_LONG) {
            wb_power_note_activity();
            wb_vokie_mode_note_activity();
            vokie_svc_key(btn, true);
            return;
        }
        return;   // RELEASE / LONG_UP 已由 CLICK/LONG 覆盖
    }

    if (event == BTN_EVT_PRESS) {
        wb_power_note_activity();
        return;   // 只用于唤醒/续期，不产生业务动作
    }

    if (event == BTN_EVT_CLICK) {
        wb_power_note_activity();
        wb_audio_set_standby(false);
        wb_view_t view;
        wb_biz_snapshot(&view);
        const wb_ui_key_t ui_key = ui_key_of(btn);
        if (wb_ui_key(ui_key, false)) {
            beep_key();
            request_redraw();
            return;
        }
        const bool approval = view.page == WB_PAGE_APPROVAL && view.approval;
        const bool allow = view.sel == 0;
        switch (btn) {
        case BSP_BTN_UP: wb_biz_move(-1); break;
        case BSP_BTN_DOWN: wb_biz_move(1); break;
        case BSP_BTN_OK: wb_biz_confirm(); break;
        default: break;
        }
        // 审批确认/拒绝单独给音；其余按键给短反馈。S3 原本两者都不响。
        if (approval && btn == BSP_BTN_OK) {
            wb_audio_play(allow ? WB_SFX_CONNECTED : WB_SFX_KEY);
        } else {
            beep_key();
        }
        return;
    }

    if (event == BTN_EVT_LONG) {
        wb_power_note_activity();
        wb_audio_set_standby(false);
        wb_view_t view;
        wb_biz_snapshot(&view);
        const wb_ui_key_t ui_key = ui_key_of(btn);
        if (wb_ui_key(ui_key, true)) {
            beep_key();
            request_redraw();
            return;
        }
        if (btn == BSP_BTN_DOWN) {
            const bool had_page = view.page != WB_PAGE_OVERVIEW;
            wb_biz_back();
            if (had_page) beep_key();   // 审批返回 / 提问 skip 的反馈
        } else if (btn == BSP_BTN_OK) {
            // 导航页的长 OK 已被 wb_ui_key 消费（回看板首页）并提前 return。
            // 走到这里的只有审批 / 提问事件页：不抢手势，保留底座“2s 提示 /
            // 5s 关机”语义，同时避免 registry 把长 OK 当“整机返回”。
            // 按满 5s 的「进 Vokie / 关机」手势由底座 power 流程接管（power.cc）。
        }
        return;
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
