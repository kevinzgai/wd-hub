// passport-fw/main/wb/wb_page_priority.h —— 决定「这一帧该显示哪一页」的纯函数。
//
// 为什么单独抽出来：
//   这条链是整块看板唯一「会打断用户」的地方（离线页、低电页会盖住任何页），
//   而它原来写死在 wb_view.c 里、依赖 LVGL，host 单测编不进来 —— 也就是说
//   改动只能靠烧录 + 人工翻页验证。上一轮加第 5 页时已经踩过这个坑（抓屏抓到
//   第 3 页/第 5 页不确定）。抽成纯 inline 之后，全组合可以由 test 穷举断言。
//
// 本头只依赖 wb_view.h（枚举）+ wd_core.h（wb_state_t），两者都是纯类型，
// 不含 LVGL / IDF，因此 host 侧可直接 include。
//
// 优先级来自 docs/KEYMAP_PLAN.md §5.1「打断优先级」，从高到低：
//   1. 待机：仅当无 RUNNING、无审批、无提问且空闲 180s —— 待机中不弹覆盖页，
//      否则用户放着不动反而被离线页反复吵；
//   2. 离线：未连接 —— 高于审批。断连时审批/提问的 key 帧根本发不出去
//      （wb_link_send 会失败），让人「答了但没生效」比不让答更糟；
//   3. 审批 / 提问：已连接且有待办，提问优先于审批；
//   4. 低电：SOC ≤ 15% 且未被屏蔽 —— 低于审批，因为低电只是提醒、不阻断上行，
//      不能让一块电池通知挡住待办；
//   5. 用户手动选择的导航页。
#pragma once

#include "wb_view.h"

#ifdef __cplusplus
extern "C" {
#endif

// 低电覆盖页阈值。与 wb_audio.c 的低电提示共用同一组数字，单点定义，
// 避免「提示音响了但页面不弹」这类两处阈值漂移。
//   <= ON  触发覆盖页
//   >  OFF 复位（回差 3%，避免在 15% 附近反复弹）
#define WB_LOWBATT_ON   15
#define WB_LOWBATT_OFF  18

// 离线覆盖页是否该显示。
// 注意：离线页**没有 dismiss 状态**，这是刻意的（见 wb_view.c 注释）——
// 断连时任何页面显示的都是旧帧，「关掉通知」等于把缓存数据当成实时数据看，
// 正是本工程要消除的误读。
static inline bool wb_page_offline_active(const wb_state_t *st)
{
    if (!st) return false;
    // 待机不进覆盖页：省电档下主机断掉是常态，弹页只会耗电且扰人。
    if (st->standby_page) return false;
    return st->offline || !st->linked;
}

// 低电覆盖页是否该显示。soc < 0 表示电量读表失败，绝不猜。
static inline bool wb_page_lowbatt_active(const wb_state_t *st, bool dismissed)
{
    if (!st) return false;
    if (st->soc < 0 || st->soc > WB_LOWBATT_ON) return false;
    if (st->standby_page) return false;   // 待机页自己已有省电说明，不叠加
    return !dismissed;
}

// 选页。manual 是用户手动选的导航页（STATUS..HOSTRES）。
// 纯函数：同输入必同输出，不读全局、不改状态、不碰 LVGL。
static inline wb_view_page_t wb_page_pick(const wb_state_t *st,
                                         wb_view_page_t manual,
                                         bool lowbatt_dismissed)
{
    if (!st) return manual;

    if (st->standby_page) return WB_VIEW_STANDBY;      // 1
    if (wb_page_offline_active(st)) return WB_VIEW_OFFLINE;  // 2
    if (st->appr.active) return WB_VIEW_APPROVAL;     // 3
    if (st->ask.active) return WB_VIEW_ASK;           // 3'
    if (wb_page_lowbatt_active(st, lowbatt_dismissed)) return WB_VIEW_LOWBATT;  // 4
    return manual;                                     // 5
}

#ifdef __cplusplus
}
#endif
