// main/wb_console.h —— WorkBuddy 控制台编排层（ESP32-S3 星智 1.54TFT）。
//
// 与 ai-passport(C3) 版的差异：那块板用 espressif/button 组件 + bsp_lvgl_lock，
// 这块板是 LVGL 单任务模型（lvgl_task 独占控件树），按键任务只写标志。
// 所以这里不再起"app 任务持锁刷界面"，改成：
//
//   BLE host 任务  → 整帧入队，立刻返回（不能阻塞 NimBLE）
//   button 任务    → 只投 (key, long) 请求
//   app 任务       → 解码帧、推进状态机、发 ACK、读电量
//   lvgl 任务      → 每帧调 wb_console_tick()，在这里刷界面（唯一碰 LVGL 的地方）
//
// 线程安全的关键：wb_view 的"游标类"函数（rack_move / ask_move …）只改 int，
// 不碰 LVGL，所以允许在 app 任务里调；真正碰控件的只有 wb_view_refresh()。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化状态机与 BLE，起内部任务。在 app_main 里调一次（LVGL 已初始化之后）。
// 失败只打日志不 abort —— 蓝牙起不来不该让整机黑屏。
void wb_console_start(void);

// 由 lvgl_task 每帧调用。内部按需切 screen、刷控件。
void wb_console_tick(int64_t now_ms);

// 控制台当前是否占用屏幕与按键（按键任务每轮现问，不缓存）。
bool wb_console_active(void);

// 按键任务投递（key: "OK" / "UP" / "DOWN"）。只入队，不阻塞。
void wb_console_key(const char *key, bool long_press);

// 双击 OK 切外观（赛博 HUD / 像素风）。落 NVS，重启保留。
void wb_console_toggle_skin(void);

#ifdef __cplusplus
}
#endif
