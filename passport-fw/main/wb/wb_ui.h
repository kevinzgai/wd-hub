// passport-fw/main/wb/wb_ui.h —— WD Hub 看板对外 API（应用 ↔ 视觉层契约）。
// 实现：wb_ui.cc 适配业务快照与三键，wb_view.c 负责方向 A「仪表像素」8 页渲染。
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
// 低电覆盖页只在 <=15% 出现，充电恢复（>18%）后自动复位 dismissed 状态。
void wb_ui_set_battery(int soc);
// 电池电压（mV），<0 表示读表失败。低电页用它说明电量计滞后的情况，
// 与 soc 分开传：soc 决定「弹不弹页」，电压只解释「为什么读数这么低」。
void wb_ui_set_battery_mv(int mv);

// 调试专用（串口 'w'）：注入假电量并冻结，用于实机验证低电覆盖页。
// 冻结后周期读表不再覆盖它；收到 >18% 的真实读数或重新烧录即解除。
void wb_ui_force_battery_debug(int soc);

// 三键分发闸门。返回 true 表示本层已消费，业务层不再处理（返回 false 时
// 短 OK 会落到 wb_biz_confirm，务必谨慎）。仅处理 CLICK / LONG；
// PRESS/RELEASE 仍由应用层做唤醒与续期。
// 导航页：短 ↑↓ 翻页、长按 OK 回首页、短 OK 吞掉（保留空位）。
bool wb_ui_key(wb_ui_key_t key, bool long_press);
// 每秒在 LVGL 上下文调用：处理菜单/声音页的 30s 超时与「立即待机」请求。
// 返回 true 表示 UI 层状态已变化，调用方需要重绘。
bool wb_ui_tick(void);

#ifdef __cplusplus
}
#endif
