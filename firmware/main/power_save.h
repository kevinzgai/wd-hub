/* 省电策略 —— 统一的电源状态切换
 *
 * 为什么单独一个模块
 * ------------------
 * 省电不是"改一个背光值"就完事，它要同时改四处：
 *   背光亮度、WiFi 省电模式、取数轮询间隔、LVGL 帧率。
 * 这四处分散在 board_display / wifi_mgr / data_fetch / main 里，
 * 各改各的话必然不同步 —— 典型症状是"背光暗了但还在每 5 秒联网"
 * （用户以为省电了，其实一点没省）。
 *
 * 所以收敛成一个状态机 + 一个 set 函数，谁都不许单独改这几项。
 *
 * 三级状态
 * --------
 *   ACTIVE  正常工作，全速
 *   LIGHT   轻度省电：空闲 3 分钟。背光 30% + 只显示大时钟。
 *           还是 5 秒拉数据 —— 用户可能随时回来，数据得是新的。
 *   DEEP    深度省电：空闲 10 分钟。背光 5% + WiFi 最大省电
 *           + 拉取间隔拉到 60 秒 + LVGL 帧率降到 500ms。
 *
 * 为什么不用 esp_pm 动态调频
 * --------------------------
 * CONFIG_FREERTOS_HZ=1000（1ms tick）会让 tickless idle 基本失效 ——
 * 系统每 1ms 被 tick 叫醒一次，进不去长 idle，DVFS 的收益会被吃掉大半。
 * 而收益不稳定、风险却很实在：CPU 降频会拉低 APB 总线频率，
 * LEDC 背光（LOW_SPEED_MODE 自动选时钟源）和 SPI 80MHz 屏驱都可能受影响。
 * 拿"省不确定的一点电"换"屏幕可能花屏"，不划算。
 *
 * 改用更朴素但确定的办法：**让它真的闲着**。
 * 轮询从 5s 拉到 60s、帧率从 20fps 降到 2fps，CPU 自然大部分时间在
 * 阻塞态，此时 IDF 会自动进 light sleep（不需要 CONFIG_PM_ENABLE）。
 *
 * 唤醒代价
 * --------
 * 从 DEEP 醒来是"改回策略"，不是重启 —— 不用重连 WiFi、不用重拉数据，
 * 1 秒内就能看到正常界面。这是相比 deep sleep 的最大优势。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    POWER_ACTIVE = 0,   // 全速
    POWER_LIGHT,        // 轻度省电（空闲 3 分钟）
    POWER_DEEP,         // 深度省电（空闲 10 分钟）
} power_level_t;

/* 初始化。必须在 board_display_init / wifi_mgr_begin 之后调用 —— 
 * 它要操作已经建好的 LEDC 和 WiFi 驱动。 */
void power_save_init(void);

/* 切换电源级别。幂等：同一级别重复调用什么都不做。
 *
 * 内部会按级别设置：背光、WiFi PS、轮询间隔、LVGL 帧率。
 * 各模块通过 power_save_* 取值函数拿到自己的参数，别自己定常量。 */
void power_save_set(power_level_t lv);

/* 每帧调一次（很便宜，就一次状态比较）。
 *
 * 存在的理由：WiFi PS 只能在连上之后设，而开机时 wifi_mgr_begin() 是
 * 异步的 —— power_save_init() 执行那一刻大概率还没连上，那次设置被跳过。
 * 之后如果一直是 ACTIVE，就再也没有"级别变化"的时机去补设，
 * 结果 WiFi 一直跑驱动默认值，醒着时切屏重拉慢一拍。
 *
 * 所以让 UI 主循环每帧问一次：连上了吗？连上了且还没设过就补一次。 */
void power_save_tick(void);

/* 当前级别 */
power_level_t power_save_level(void);

/* ---- 各模块取参数用 ---- */

/* 取数轮询间隔（毫秒）。ACTIVE/LIGHT 是 5s，DEEP 是 60s。 */
uint32_t power_save_poll_ms(void);

/* LVGL 主循环最长等待（毫秒）。ACTIVE 50，LIGHT 100，DEEP 500。 */
uint32_t power_save_lvgl_max_wait_ms(void);

/* 背光百分比。ACTIVE 由用户档位决定（不走这里），LIGHT 30，DEEP 5。 */
uint32_t power_save_backlight_pct(void);
