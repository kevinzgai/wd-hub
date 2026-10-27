/* 像素宠物视图（右下角的猫）
 *
 * 职责：建图、按状态换帧、根据数据变化切状态、以及"什么时候该显示"的
 * 落地动作。素材和状态枚举在 pet.h，本模块只管把它画出来。
 *
 * 从 main.cpp 拆出：宠物是纯装饰，却占着 500 多行，和看板主渲染混在一起
 * 时改个表情要翻半天。拆开之后 main 只需要在切屏/休眠时告诉它显不显示。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "board_data.h"

/* 建图（挂到 scr 上）并压到最底层、起换帧定时器。
 * 必须在 ui_build 里、其它内容控件建完之后调用一次。 */
void pet_view_build(lv_obj_t *scr);

/* 根据一轮数据更新状态机 + 触发提示音（完成 / 会话收工 / 等你操作）。
 * 非第 0 屏直接跳过 —— 详情屏不带 working/today_done，参与判断会误响。 */
void pet_view_update(const board_data_t *d);

/* 强制某个状态（离线时回到待机表情，表示"活着但没活干"）。 */
void pet_view_set_state(uint8_t st);

/* 显示 / 隐藏。隐藏时不换帧（省 SPI）；重新显示会立刻补一帧，
 * 让表情马上对上当前状态，而不是等下一个换帧周期。 */
void pet_view_set_visible(bool on);
