/* 屏幕驱动：SPI + ST7789 + LVGL 显示对象 + 背光
 *
 * 从 main.cpp 拆出来：这一层只管"怎么把像素打到屏上、怎么调背光"，
 * 完全不知道屏上画的是什么（那是 ui_board 的事）。
 *
 * 拆分的理由：显示初始化是**一次性、不可重入**的代码（SPI 总线、面板句柄、
 * DMA 帧缓冲），和会反复跑、逻辑复杂的 UI 混在一个文件里时，
 * 改布局很容易误碰这里的时序。分开之后 ui_board 改多少次都不影响这块。
 */
#pragma once

#include <stdint.h>

/* 初始化 SPI 总线、ST7789 面板、LVGL 显示对象和 DMA 帧缓冲。
 *
 * ⚠ 必须在 lv_init() 之后调用 —— 内部要调 lv_display_create /
 *   lv_display_set_buffers，LVGL 内部结构还没初始化就往堆里写会直接踩坏堆
 *   （现象：assert failed: remove_free_block tlsf_control_functions.h:372）。
 *
 * 帧缓冲分配失败直接 abort：屏幕没法画，继续跑也只是白跑。 */
void board_display_init(void);

/* 设置背光亮度（0..100，越界按 100 处理）。 */
void board_display_backlight_set(uint32_t percent);
