// main/console/wb_view.h —— WorkBuddy 看板 7 页(240x320)。
//
// 页面集合与旧的 wb_view.c 六页结构对齐，并按本机“三键 + 320 高”补一页：
//   0 状态/总览   1 会话列表   2 今日消耗   3 关于/链路
//   4 审批(事件)  5 提问(事件) 6 待机(空闲自动)
//
// 线程约定：所有函数都必须在持有 bsp_lvgl_lock 的 LVGL 上下文里调用；
// 本模块不缓存 lv_screen_active()，建屏/删屏由 console App 的 enter/exit 驱动。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_VIEW_STATUS = 0,
    WB_VIEW_RACK,
    WB_VIEW_USAGE,
    WB_VIEW_ABOUT,
    WB_VIEW_APPROVAL,
    WB_VIEW_ASK,
    WB_VIEW_STANDBY,
    WB_VIEW_COUNT,
} wb_view_page_t;

// 建/删 7 个 screen 与全部控件。调用方持有 LVGL 锁。
void wb_view_build(void);
void wb_view_teardown(void);

// 按状态机快照选页、切页并刷新控件。调用方持有 LVGL 锁。
void wb_view_refresh(const wb_state_t *st, int64_t now_ms);

// 当前应显示的页(不含强制页)。
wb_view_page_t wb_view_current_page(const wb_state_t *st);

// 手动导航页(STATUS/RACK/USAGE/ABOUT)。事件页/待机页优先级更高。
void wb_view_set_manual_page(wb_view_page_t page);
wb_view_page_t wb_view_manual_page(void);

// 提问页游标。只改 int，不碰 LVGL，允许在 console 任务里调。
void wb_view_ask_move(int delta, int n_opts);
int  wb_view_ask_cursor(void);
void wb_view_set_ask_cursor(int cursor);

// 提问页交互态（只写 int，渲染在下次 wb_view_refresh 里生效）：
//   committed —— 已确认的题号与选项下标，id<=0 表示清除。用于把落选项画成
//                绿色并压暗其余项，同时告知用户「提交的是哪一项」。
//   edge      —— +1 撞末项 / -1 撞首项 / 0 清除，until_ms 到期后恢复默认
//                页脚文案。游标本身由业务层钳制，这里只负责提示。
void wb_view_set_ask_committed(int id, int idx);
void wb_view_set_ask_edge(int dir, int64_t until_ms);

// 给“关于/链路”页补充服务层统计(下行帧数、最近事件、BLE 省电档)。
void wb_view_set_activity(int frames, const char *last_event, bool low_power);

#ifdef __cplusplus
}
#endif
