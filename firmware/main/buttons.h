/* 按键扫描（BOOT / VOL+ / VOL-）
 *
 * 只做三件事：消抖、判定"短按 / 长按 / 长按 3 秒"、调对应回调。
 * 具体动作（切屏、调亮度、进配网）通过 hooks 交给上层 ——
 * 所以这个模块不碰任何 UI 状态，改按键语义不用动它。
 *
 * 三种语义按当时的网络状态自动分流（见 README「按键的三种语义」）：
 *
 *   | 操作        | 联网正常        | WiFi 连不上 | 服务连不上       |
 *   | 短按        | 下一屏          | 下一屏      | 下一屏           |
 *   | 长按 3 秒   | 调亮度（700ms起）| 进配网      | 重开服务地址配置页 |
 *
 * 长按在联网时不会进配网：配网会中断数据轮询，联网状态下不该被误触。
 */
#pragma once

#include <stdbool.h>

typedef struct {
    /* ---- 状态查询（决定长按分流到哪条语义）---- */
    bool (*wifi_connected)(void);
    bool (*svr_down)(void);
    bool (*is_asleep)(void);        /* 省电模式：短按只唤醒，不切屏 */

    /* ---- 动作 ----
     * 全部在按键任务里被调用，**不要在里面直接动 LVGL** ——
     * 上层只写请求标志，由 LVGL 任务消费。 */
    void (*wake)(void);
    void (*next_page)(void);
    void (*brightness)(const char *key, int step);  /* step: 0=循环, +1/-1=增减 */
    void (*provision)(void);
    void (*svr_config)(void);
} buttons_hooks_t;

/* 配好引脚并起扫描任务（20ms 一轮）。hooks 必须比任务活得久（用静态变量）。 */
void buttons_start(const buttons_hooks_t *hooks);
