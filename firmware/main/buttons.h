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
#include <stdint.h>

/* 长按判定阈值（消抖确认之后的按住时长）。
 *
 * 公开出来是因为上层也要用同一个数：第 6 屏"按住说话"在松手时得判断
 * 这次按压到底算"按住说话"还是"短按切目标"，判据必须和这里完全一致 ——
 * 两处各写一个 700，改一处忘另一处就会出现"界面认为是短按、录音却发出去了"。 */
#define BTN_LONG_MS 700

typedef struct {
    /* ---- 状态查询（决定长按分流到哪条语义）---- */
    bool (*wifi_connected)(void);
    bool (*svr_down)(void);
    bool (*is_asleep)(void);        /* 省电模式：短按只唤醒，不切屏 */

    /* ---- 控制台接管（WorkBuddy 控制台页面显示时）----
     *
     * console_active() 返回 true 时，**所有**按键（短按与长按）都改投给
     * console_key()，本模块不再做切屏/亮度/配网的分流。
     *
     * 为什么在按键层分流而不是让上层自己判：长按的语义在这里判定
     * （700ms 阈值 + fired 去重），上层拿不到"这次按压是否已按长按处理过"。
     * 若让上层自己计时，会和控制台的页面切换互相打架。
     *
     * 两个回调都在按键任务里调用，**不要碰 LVGL** —— 控制台侧只写请求标志。 */
    bool (*console_active)(void);

    /* 按下（消抖通过）那一刻的通知。**当前固件不用**（语音上行已下线，
     * hooks 里是 nullptr），槽位保留：以后若要恢复"按住说话"这类需要
     * t=0 通知的交互，直接填回来即可。 */
    void (*console_press)(void);

    void (*console_key)(const char *key, bool long_press);   /* key: "OK" / "UP" / "DOWN" */

    /* 松开那一刻的通知，held_ms = 这次按压总共按了多久。**当前固件不用**。
     * console_key() 只能表达"短按"和"长按已发生"两个时刻，拿不到
     * "手指抬起来了"；按住类交互（如按住说话、松开发送）靠它收尾。
     * 每次有效松开都会调（包括长按之后），由上层自己判断要不要处理。 */
    void (*console_release)(uint32_t held_ms);

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
