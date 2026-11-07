// main/console/wb_view.h —— WorkBuddy 看板 8 页(240x320)。
//
// 页面集合与旧的 wb_view.c 六页结构对齐，并按本机“三键 + 320 高”补两页：
//   0 状态/总览   1 会话列表   2 今日消耗    3 关于/链路
//   4 主机资源   5 审批(事件)  6 提问(事件)  7 待机(空闲自动)
//
// 导航页(↑↓ 轮转)只有前 5 页(0..4)；审批/提问是事件页，待机是空闲覆盖页，
// 都不参与轮转。

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
    // ---- 导航页（↑↓ 轮转，0..WB_VIEW_HOSTRES 连续）----
    WB_VIEW_STATUS = 0,
    WB_VIEW_RACK,
    WB_VIEW_USAGE,
    WB_VIEW_ABOUT,
    WB_VIEW_HOSTRES,   // 主机资源：电脑 CPU / 内存 / 磁盘（导航页第 5 页）
    // ---- 非导航页 ----
    WB_VIEW_APPROVAL,  // 事件页：已连接且有待审批
    WB_VIEW_ASK,       // 事件页：已连接且有待作答
    WB_VIEW_STANDBY,   // 空闲 180s 覆盖页
    WB_VIEW_OFFLINE,   // 断连覆盖页（不可 dismiss，重连自动消失）
    WB_VIEW_LOWBATT,   // 低电覆盖页（SOC <= 15%，可关闭，充电后复位）
    WB_VIEW_COUNT,
} wb_view_page_t;

// 导航页数量（STATUS..HOSTRES 连续）。覆盖页/事件页插在 HOSTRES 之后，
// 因此「导航页上界」这个常量必须显式写出来，不能再用 WB_VIEW_APPROVAL 推导。
#define WB_NAV_PAGE_COUNT ((int)WB_VIEW_HOSTRES + 1)

// 建/删全部 screen 与控件（按需只保留当前一屏）。调用方持有 LVGL 锁。
void wb_view_build(void);
void wb_view_teardown(void);

// 按状态机快照选页、切页并刷新控件。调用方持有 LVGL 锁。
void wb_view_refresh(const wb_state_t *st, int64_t now_ms);

// 当前应显示的页（含覆盖页与事件页的强制优先级）。
// 判定逻辑在 wb_page_priority.h 的 wb_page_pick()，纯函数、可单测。
wb_view_page_t wb_view_current_page(const wb_state_t *st);

// 手动导航页(STATUS/RACK/USAGE/ABOUT/HOSTRES)。覆盖页/事件页优先级更高。
void wb_view_set_manual_page(wb_view_page_t page);
wb_view_page_t wb_view_manual_page(void);

// 导航页数量（STATUS..HOSTRES 连续 5 页）。调用方用它做 ↑↓ 轮转上界，
// 避免各处自己再写一遍 WB_VIEW_ABOUT+1 这类常量。
int wb_view_nav_page_count(void);

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

// 低电覆盖页的「关闭」状态。低电只是提醒，用户可以关掉继续用；
// 充到 >18% 会自动复位，下次再掉到 <=15% 会重新弹出。
// 与离线页不同：离线页没有 dismiss —— 断连时任何页显示的都是旧帧，
// 「关掉通知」等于让人把缓存当成实时数据看。
void wb_view_set_lowbatt_dismissed(bool dismissed);
bool wb_view_lowbatt_dismissed(void);

// 注：原 wb_view_set_activity(frames, last_event, low_power) 已移除。
// 它唯一的调用点写死在 init 里，屏上「0 帧 / 正常档」恒不变。现在链路统计
// 走 wb_link_stats_t（wd_core.h 的 wb_state_t::link），由 wb_ui.cc 每帧搬运。

#ifdef __cplusplus
}
#endif
