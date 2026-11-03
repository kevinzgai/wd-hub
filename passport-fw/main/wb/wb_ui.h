// passport-fw/main/wb/wb_ui.h —— 240x320 占位视觉层（坐标/字号只在本层出现）。
#pragma once

#include "lvgl.h"
#include "wb_biz.h"

#ifdef __cplusplus
extern "C" {
#endif

// 必须在 LVGL 上下文调用（应用 start / 1s tick）。
void wb_ui_init(void);
void wb_ui_deinit(void);
void wb_ui_sync(const wb_view_t *view, bool standby);

#ifdef __cplusplus
}
#endif
