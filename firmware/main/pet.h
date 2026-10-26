// 像素宠物素材定义
//
// 素材本体由 tools/gen_pet.py 生成到 pet_frames.c，这里只放共用常量。
//
// 构图：参考图（猫耳机器人）只取**头部**，手绘成 40x32 像素画，
// 3 倍整数放大到 120x96。
//
// 为什么只取头部而不做全身像：
//   参考图暗部占 70%（面罩/马甲/裤子），屏底色 #06080C、宠物又是
//   LV_OPA_40 半透明压在最底层。全身像缩到 120px 后暗部与背景同色，
//   实测只剩"一坨黑"，除了猫耳和青眼镜什么都看不出。头部是唯一
//   含有高对比结构的区域（白头盔 + 青眼镜 + 深面罩），辨识度最高。
//
// 素材就是 120x96，**没有**上下留白 —— 之前这里曾声称"纵向居中放进
// 120x120 画布、上下各留 12px 透明"，但描述符实际是 .h=96、data_size=46080，
// 根本没有 PAD。那段说法是错的，PET_PAD_Y 也从未被引用过，已删除。
// 现在 BOTTOM_RIGHT 对齐后占 y=148..243，底部 4px 出屏（见 main.cpp）。
//
// 运行时不做任何缩放：lv_image_set_scale 只改绘制变换、不改对象尺寸，
// 放大后图像会以 pivot(默认中心)为原点溢出对象边界被裁掉，只剩左上角。
#pragma once

#include "lvgl.h"
#include <stdint.h>

#define PET_SRC_W       40                            // 像素画宽（列）
#define PET_SRC_H       32                            // 像素画高（行）
#define PET_PIX         3                             // 每逻辑像素的绘制倍数
#define PET_IMG_W       (PET_SRC_W * PET_PIX)         // 120，素材实际宽
#define PET_IMG_H       (PET_SRC_H * PET_PIX)         //  96，素材实际高

#define PET_FRAMES_EACH 2     // 每种状态 2 帧循环
#define PET_STATES      4     // idle / working / done / auto
#define PET_FRAME_COUNT (PET_STATES * PET_FRAMES_EACH)

// 状态编号。必须与 gen_pet.py 里 FRAMES 的顺序严格一致，
// 因为 pet_frames[] 就是按这个顺序排的：index = state * PET_FRAMES_EACH + frame
enum {
    PET_ST_IDLE = 0,   // 待机：没人在干活
    PET_ST_WORKING,    // 工作中：有 session 在跑
    PET_ST_DONE,       // 完成：刚有任务收工
    PET_ST_AUTO,       // 等你操作：有会话卡在等待确认
};

// 8 张帧描述符（ARGB8888，透明像素 alpha=0，可安全叠在文字上方）
extern const lv_image_dsc_t pet_frames[PET_FRAME_COUNT];
