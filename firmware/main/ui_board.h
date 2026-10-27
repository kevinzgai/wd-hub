/* 看板 UI：控件树 + 渲染 + 交互状态机
 *
 * 从 main.cpp 拆出来：这一层负责"屏上长什么样、现在显示哪一屏"，
 * 不碰网络、不碰 SPI、不碰按键扫描。
 *
 * ## 对外接口只有五类
 *
 *   1. 建屏      ui_board_build()
 *   2. 渲染      ui_board_render(const board_data_t *d)
 *   3. 每帧轮询  ui_board_poll()  —— 浮层状态同步
 *   4. 查询      ui_board_page() / ui_board_asleep() / ui_board_last_btn()
 *   5. 动作      ui_board_next_page() / ui_board_wake() / ui_board_brightness()
 *
 * ## 为什么主文件要"问"而不是"被推"
 *
 * 取数任务每轮都要知道"现在该拉第几屏"，按键任务要知道"是否在省电模式"。
 * 这些都是 UI 的内部状态，所以这里用取值函数（函数指针）暴露给调用方，
 * 调用方每轮现问、不缓存 —— 避免两个任务各存一份、切屏后不同步。
 *
 * ## 线程约定
 *
 * 所有涉及 LVGL 的函数都**必须在 LVGL 任务里调用**。
 * 按键任务只调 ui_board_next_page() / ui_board_wake() / ui_board_brightness()，
 * 这三个函数只写请求标志，由 LVGL 任务消费（见 main.cpp 的 lvgl_task）。
 */
#pragma once

#include <stdbool.h>

#include "board_data.h"

/* 建屏：创建全部控件、挂上宠物、起闪烁定时器。
 * 必须在 lv_init() + board_display_init() 之后调用一次。 */
void ui_board_build(void);

/* 用一份数据快照刷新画面（概览屏 / 列表屏 / 图表屏 / 休眠屏）。
 * 不持数据锁 —— 调用方负责先 memcpy 出快照再传进来。 */
void ui_board_render(const board_data_t *d);

/* 每帧轮询，分两段。分成两段是为了让**网络动作**夹在中间 ——
 * 配网/重开配置页会改 wifi_mgr 的状态，浮层同步必须排在它之后，
 * 否则屏幕上会晚一帧才显示新状态。拆分前 main.cpp 里的顺序就是这样：
 *
 *   poll_input()  →  配网/配置页动作  →  poll_status()  →  渲染
 */

/* 消费按键请求：亮度、切屏、唤醒。 */
void ui_board_poll_input(void);

/* 同步 WiFi 浮层、服务地址浮层、按键提示。
 * 内部有状态去重，状态没变时不会碰控件，可以随便调。 */
void ui_board_poll_status(void);

/* ---- 查询 ---- */

/* 当前屏号（0 = 概览屏）。取数任务每轮现问，用来决定拉 /status 还是 /screen/<i>。 */
int  ui_board_page(void);

/* 是否处于省电模式（空转休眠）。按键任务用它决定"短按是切屏还是唤醒"。 */
bool ui_board_asleep(void);

/* 最后按下的键名，仅用于日志（"btn=BOOT"）。 */
const char *ui_board_last_btn(void);

/* ---- 动作（由按键任务调用，只写请求标志，不碰 LVGL）---- */

/* 请求切到下一屏。由 LVGL 任务落实。 */
void ui_board_next_page(void);

/* 请求退出省电模式（并清空闲计时）。由 LVGL 任务落实。 */
void ui_board_wake(void);

/* 调整背光档位。step: 0 = 循环（亮→暗→亮），+1 = 调亮，-1 = 调暗。 */
void ui_board_brightness(const char *key, int step);

/* 当前背光档位的百分比（40 / 70 / 100）。开机点亮背光时用。 */
int ui_board_brightness_pct(void);
