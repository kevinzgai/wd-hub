// main/wb_eyes.h —— 眼睛动画（原 ui_board 休眠屏的逐像素大眼睛，抽成共用模块）。
//
// 从 ui_board.cpp 抽出来的原因：控制台（wb_view）第 6 页也要用它。
// 原来它嵌在 ui_board 的休眠屏里，私有 static 变量 + 私有 timer，
// 控制台想复用只能复制一份 —— 而这份代码全是像素算法的细节（抗锯齿、
// 睑线、瞳孔裁切），复制两份等于以后改一处忘一处。
//
// 用法（调用方全程待在 LVGL 任务里，本模块不自己起任务）：
//   wb_eyes_build(某个 lv_obj 容器);      // 建画布 + 停着的定时器
//   wb_eyes_enter();                      // 进待机页时：重置 + 启动动画
//   wb_eyes_leave();                      // 离开时：暂停，不再算一帧
//
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 画布在父容器内的位置与尺寸（设备坐标，240x240 屏）。
// 上下各留余量给呼吸浮动：眼睛本体 y=39..93，画布覆盖 22..104。
#define WB_EYES_X   18
#define WB_EYES_Y   22
#define WB_EYES_W  204
#define WB_EYES_H   82

// 建画布。父容器会被眼睛所在的那一页持有（随页显示/隐藏）。
// 返回 false = PSRAM 分配失败（调用方应退化成"无眼睛"的纯文字待机页，
// 不能把它当致命错误 —— 为了一对眼睛把整机拖垮不值得）。
//
// ⚠ 父容器被删（切外观会 teardown 重建整套界面）时，画布随之销毁，
// 但 s_eye_buf 和 s_eye_timer 还活着。所以**必须先调 wb_eyes_free()**，
// 否则这里会因为"看起来已经建过"而直接返回 true，指向一个已释放的控件。
bool wb_eyes_build(lv_obj_t *parent);

// 释放画布与定时器（父容器即将被删时调用）。幂等，可重复调。
void wb_eyes_free(void);

// 进/出待机页。enter 会重置状态并从"闭眼"开始睁眼（像被叫醒）；
// leave 暂停定时器，此后一帧都不算。
void wb_eyes_enter(void);
void wb_eyes_leave(void);

// 当前是否已建好（没建好时调用 enter/leave 是安全的空操作）。
bool wb_eyes_ready(void);

#ifdef __cplusplus
}
#endif
