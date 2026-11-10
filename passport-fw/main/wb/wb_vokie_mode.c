// passport-fw/main/wb/wb_vokie_mode.c —— Vokie 模式状态机实现。
#include "wb_vokie_mode.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "launcher/system_overlay.h"
#include "wb/vokie_svc.h"

static const char *TAG = "vokie_mode";

#define VOKIE_MODE_TIMEOUT_US (60LL * 1000000LL)

static bool s_active;
static bool s_changed;               // 切换沿，app_wb tick 消费
static int64_t s_last_activity_us;

bool wb_vokie_mode_active(void) { return s_active; }

void wb_vokie_mode_note_activity(void) { s_last_activity_us = esp_timer_get_time(); }

void wb_vokie_mode_enter(void)
{
    if (s_active) return;
    if (!vokie_svc_host_ready()) {
        ESP_LOGW(TAG, "Vokie 主机未就绪，留在看板模式");
        system_overlay_show("Vokie 未连接\n打开 Vokie 后重试", 2500);
        return;
    }
    s_active = true;
    s_changed = true;
    wb_vokie_mode_note_activity();
    ESP_LOGI(TAG, "进入 Vokie 模式");
}

void wb_vokie_mode_exit(void)
{
    if (!s_active) return;
    s_active = false;
    s_changed = true;
    vokie_svc_cancel();              // 录音中先正常收尾（ptt_up）
    ESP_LOGI(TAG, "退回看板模式");
}

bool wb_vokie_mode_poll_expired(void)
{
    if (!s_active) return false;
    if (esp_timer_get_time() - s_last_activity_us < VOKIE_MODE_TIMEOUT_US) return false;
    ESP_LOGI(TAG, "60s 无操作，自动退回看板模式");
    wb_vokie_mode_exit();
    return true;
}

bool wb_vokie_mode_take_changed(bool *active_now)
{
    if (!s_changed) return false;
    s_changed = false;
    if (active_now) *active_now = s_active;
    return true;
}
