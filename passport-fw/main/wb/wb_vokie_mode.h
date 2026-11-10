// passport-fw/main/wb/wb_vokie_mode.h —— Vokie 模式状态机。
//
// 交互（2026-11-10 定）：
//   WB 看板模式长按 OK ≥2s 松手 → 进入 Vokie 模式（按满 5s 仍关机）；
//   Vokie 模式长按 OK ≥2s 松手 → 退回看板模式（按满 5s 仍关机）；
//   Vokie 模式内 60s 无按键 / 无状态变化 → 自动退回看板模式。
// 状态只有两枚布尔 + 一个活动时间戳，切换动作由 launcher/power.cc 触发。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 进入 Vokie 模式。Vokie 主机未就绪时弹「未连接」提示并保持看板模式。
void wb_vokie_mode_enter(void);

// 退回看板模式（录音中会先取消会话）。幂等。
void wb_vokie_mode_exit(void);

bool wb_vokie_mode_active(void);

void wb_vokie_mode_note_activity(void);

// 1s tick（LVGL 上下文）轮询：活动超时自动退出。返回 true 表示本次退出了。
bool wb_vokie_mode_poll_expired(void);

// app_wb 用：模式切换沿检测（进入或退出都算）。
bool wb_vokie_mode_take_changed(bool *active_now);

#ifdef __cplusplus
}
#endif
