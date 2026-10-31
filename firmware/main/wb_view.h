// main/wb_view.h —— 控制台界面层(240x240,LVGL 9.6)。
//
// 与看板 UI(ui_board)并存：本模块自带 6 个独立 lv_screen，按状态机决定
// 当前该显示哪一页。看板的 screen 不动，切回来时原样恢复。
//
// ⚠ 线程约定：本模块所有函数都必须在 LVGL 任务里调用（与 ui_board 一致）。
//   按键任务/BLE 任务只写请求标志，由 lvgl 任务消费。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wb_core.h"

// 建 6 个 screen 与全部控件。必须在 lv_init() + board_display_init() 之后调用。
void wb_view_build(void);

// 销毁本模块创建的全部 screen（切外观时重建用）。
void wb_view_teardown(void);

// 用状态机快照刷新当前页，并按 wb_core_page() 切换 screen。
// 内部对每处文字做"内容相同就不重排"，可每帧调用。
void wb_view_refresh(const wb_state_t *st, int64_t now_ms);

// 当前是否停在控制台某一页（false = 已退回看板屏）。
bool wb_view_active(void);

// 请求退回看板屏 / 重新进入控制台屏。由 app 层在蓝牙断开超时后调用。
void wb_view_set_active(bool on);

// 常驻屏：true 之后不再等"主机推数据"才抢屏，开机即占屏。
// 只走蓝牙的固件（WB_BLE_ONLY）用它 —— 那种板子没有看板可退。
// 关掉它（false）不影响当前所在页，只恢复"要 linked 才自动进"。
void wb_view_set_always_on(bool on);

// 强制显示某一页（page < 0 = 取消，回到状态机决定）。
// 只在版式自检（cmake -DWB_PREVIEW_DATA=1）时用：没有主机推帧时，
// 审批/提问/发语音三页永远不会出现，但它们的排版同样需要验证。
void wb_view_force_page(int page);

// ---- 外观 ----
void    wb_view_set_skin(uint8_t skin);
uint8_t wb_view_skin(void);

// ---- 交互游标（按键改这些值，刷新时读）----
// 会话列表的选中游标已在 2026-10-31 删除：短按改成翻页后没有手势
// 能移动它，那个高亮变成了纯装饰。现在列表卡的边框表示"这条在跑"。
void wb_view_ask_move(int delta, int n_opts);
int  wb_view_ask_cursor(void);

// 第 6 屏的"投递目标"游标不在这里 —— 它存在 wb_state_t.voice.target 里，
// 因为控制台任务（切目标）和刷新（高亮）都要读，多存一份只会两边不同步。
