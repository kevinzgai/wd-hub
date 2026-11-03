// passport-fw/main/wb/wb_ui.h —— V2「大字流」视觉层（坐标/字号只在本层出现）。
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

#ifdef __cplusplus
}
#endif
