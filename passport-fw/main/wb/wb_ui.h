// passport-fw/main/wb/wb_ui.h —— WD Hub 看板对外 API（应用 ↔ 视觉层契约）。
// 实现：wb_ui.cc 适配业务快照与三键，wb_view.c 负责方向 A「仪表像素」7 页渲染。
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "wb_biz.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_UI_KEY_UP = 0,
    WB_UI_KEY_DOWN,
    WB_UI_KEY_OK,
} wb_ui_key_t;

// 必须在 LVGL 上下文调用（应用 start / 1s tick）。
void wb_ui_init(void);
void wb_ui_deinit(void);
void wb_ui_sync(const wb_view_t *view, bool standby);

// 电量由应用层周期性读表后喂给 UI；<0 表示无效。
// 低电覆盖页只在 <=15% 出现，充电恢复后自动复位。
void wb_ui_set_battery(int soc);

// 离线/低电覆盖页的按键闸门。返回 true 表示本层已消费，业务层不再处理。
// 仅处理 CLICK / LONG；PRESS/RELEASE 仍由应用层做唤醒与续期。
bool wb_ui_key(wb_ui_key_t key, bool long_press);

// 每秒在 LVGL 上下文调用：处理菜单/声音页的 30s 超时与「立即待机」请求。
// 返回 true 表示 UI 层状态已变化，调用方需要重绘。
bool wb_ui_tick(void);

#ifdef __cplusplus
}
#endif
