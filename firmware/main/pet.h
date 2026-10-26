// 像素宠物素材定义
//
// 素材本体由 tools/gen_pet.py 生成到 pet_frames.c，这里只放共用常量。
// 逻辑画布 20x20，每个像素绘制成 6x6 -> 素材 120x120，与屏宽 240 的一半一致。
// 运行时不做任何缩放：lv_image_set_scale 只改绘制变换、不改对象尺寸，
// 放大后图像会以 pivot(默认中心)为原点溢出对象边界被裁掉，只剩左上角。
#pragma once

#include "lvgl.h"
#include <stdint.h>

#define PET_SRC_SIZE    20                            // 字符画边长
#define PET_PIX         6                             // 每逻辑像素的绘制倍数
#define PET_DISP        (PET_SRC_SIZE * PET_PIX)      // 120，素材边长 = 上屏边长
#define PET_SHOW_SIZE   PET_DISP                      // 与上屏尺寸一致，保留作语义区分

#define PET_FRAMES_EACH 2     // 每种状态 2 帧循环
#define PET_STATES      4     // idle / working / done / auto
#define PET_FRAME_COUNT (PET_STATES * PET_FRAMES_EACH)

// 状态编号。必须与 gen_pet.py 里 ORDER 的顺序严格一致，
// 因为 pet_frames[] 就是按这个顺序排的：index = state * PET_FRAMES_EACH + frame
enum {
    PET_ST_IDLE = 0,   // 待机：没人在干活
    PET_ST_WORKING,    // 工作中：有 session 在跑
    PET_ST_DONE,       // 完成：刚有任务收工
    PET_ST_AUTO,       // 等你操作：有会话卡在等待确认
};

// 8 张帧描述符（ARGB8888，透明像素 alpha=0，可安全叠在文字上方）
extern const lv_image_dsc_t pet_frames[PET_FRAME_COUNT];
