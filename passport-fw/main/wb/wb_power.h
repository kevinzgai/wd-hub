// passport-fw/main/wb/wb_power.h —— 待机/背光/唤醒策略。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WB_STANDBY_IDLE_SEC 180

typedef void (*wb_power_ui_cb_t)(bool standby);

void wb_power_init(wb_power_ui_cb_t on_ui_mode);

// 有新任务/活跃会话/审批/提问/按键时调用：重置空闲计时；若在待机则立刻唤醒。
void wb_power_note_activity(void);

// 有 RUNNING 会话或未处理审批/提问 → busy=true，此时不进入待机。
void wb_power_set_busy(bool busy);

bool wb_power_is_standby(void);

// 由 UI 的 1s tick 调用（LVGL 上下文）。
void wb_power_tick(void);

// 按键唤醒（等同 note_activity，但语义更明确）。
void wb_power_wake(void);

// 立即进入待机（菜单「待机」项）。必须在 LVGL 上下文调用。
void wb_power_standby_now(void);

#ifdef __cplusplus
}
#endif
