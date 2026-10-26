#!/usr/bin/env python
"""生成像素宠物的 C 素材（firmware/main/pet_frames.c）。

为什么用 ARGB8888 而不是 RGB565 预合成背景
-------------------------------------------
上一版把透明格预合成成背景色（和 logo_wb.c 同一套路），省内存也免解码。
但宠物现在要"半透明浮动"—— 预合成方案下，透明区会变成一块半透明黑罩，
把底下的文字压暗，看起来像蒙了层灰纱。

要"只有猫半透明、其他完全透"就必须带真正的 alpha 通道。
ARGB8888 的 sw 渲染器支持已开（CONFIG_LV_DRAW_SW_SUPPORT_ARGB8888=y）。

尺寸策略：直接生成 120×120（屏 240px 的一半），运行时不做任何缩放。

为什么不省这 345KB —— 踩过的坑：
上一版生成 60×60 然后 lv_image_set_scale(512) 想放大到 120。
结果屏幕上宠物只露出左上角一块。原因是 lv_image_set_scale 只改"绘制变换"，
**对象自身尺寸仍然是 60×60**，而 pivot 默认在图像中心，放大 2 倍后图像
以中心向四周各溢出 30px，超出对象边界和屏幕边缘就被裁掉。

要绕过它得手动 set_pivot(0,0) + set_size(120,120)，依赖 LVGL 内部
变换区域的实现细节，脆弱且难排查。直接在生成阶段画成 120 就是零变换、
零溢出、零歧义。

代价：ARGB8888 是 32 位，8 帧 120×120 共 460800B。
16MB 片子完全放得下（app 分区 3MB，加完还剩约 1.1MB）。

像素画怎么改
------------
改下面的 EAR_* / EYE_* / MOUTH_* / LAPTOP，改完重跑：
    python tools/gen_pet.py
逻辑画布 20×20，PIX=6 六倍绘制成 120×120。
每块像素 6×6，马赛克感明显 —— 这正是像素宠物该有的样子，
而且天然没有缩放插值造成的糊边。

    .  透明（填背景色）
    W  身体（白）      #E6EDF3  COL_TEXT
    K  眼睛/嘴（挖洞）  #0B0E14  与背景同色，在身体上打洞
    P  内耳/鼻/口      #F85149  COL_ERR
    B  笔记本（工作）   #58A6FF  COL_ACCENT

四种状态靠"有没有笔记本 + 眼神 + 耳朵"区分：
    idle     无笔记本，睁眼，慢眨眼
    working  身前有蓝色笔记本，眯眼盯着屏幕
    done     无笔记本，眼睛弯成笑眼，张口
    auto     无笔记本，耳朵被压扁一下（后台在跑）

所有行必须左右镜像对称（col c == col 19-c）。validate() 会逐行检查，
画歪了直接报错，不至于上屏才发现。
"""
import io
import os

# 索引 0 是"透明"：现在输出 ARGB8888，alpha=0 即可，不需要预合成背景色。
# 留个占位 RGB 只为让 PALETTE 下标对齐。
PALETTE = [
    (0x00, 0x00, 0x00),      # 0 透明（alpha=0，RGB 无意义）
    (0xE6, 0xED, 0xF3),      # 1 W 身体
    (0x0B, 0x0E, 0x14),      # 2 K 眼/嘴（洞）
    (0xF8, 0x51, 0x49),      # 3 P 内耳/鼻/口
    (0x58, 0xA6, 0xFF),      # 4 B 笔记本
]

CHAR_MAP = {
    '.': -1,   # 透明
    'W': 1,
    'K': 2,
    'E': 2,    # 眼睛同 K
    'P': 3,
    'B': 4,
}

SIZE  = 20      # 逻辑画布边长（字符画）
PIX   = 6       # 每个逻辑像素绘制成 6x6，得 120x120
DISP  = SIZE * PIX            # 120，素材实际边长 = 屏宽一半

# ---------------------------------------------------------------------------
# 骨架。行号分工：
#   0-3   耳朵（auto 会压扁）
#   4-13  头
#   14-19 身体（working 会被笔记本覆盖）
# ---------------------------------------------------------------------------

EAR_TALL = [
    ".....WW......WW.....",   # 0 耳尖
    "....WWWW....WWWW....",   # 1
    "....WPPW....WPPW....",   # 2 内耳
    "...WWWWWW..WWWWWW...",   # 3 耳根并入头顶
]

# 压扁：整体下移一行，视觉上"被按了一下"
EAR_FLAT = [
    "....................",
    ".....WW......WW.....",
    "....WWWW....WWWW....",
    "....WPPW....WPPW....",
]

# 头身骨架。7/8 是眼睛、10/11 是口鼻，由下面的片段覆盖
BODY = {
    4:  "..WWWWWWWWWWWWWWWW..",
    5:  ".WWWWWWWWWWWWWWWWWW.",
    6:  ".WWWWWWWWWWWWWWWWWW.",
    7:  ".WWWWWWWWWWWWWWWWWW.",   # 眼睛行 1
    8:  ".WWWWWWWWWWWWWWWWWW.",   # 眼睛行 2
    9:  ".WWWWWWWWWWWWWWWWWW.",
    10: ".WWWWWWWWWWWWWWWWWW.",   # 鼻
    11: ".WWWWWWWWWWWWWWWWWW.",   # 口
    12: "..WWWWWWWWWWWWWWWW..",
    13: "...WWWWWWWWWWWWWW...",
    14: "....WWWWWWWWWWWW....",   # 脖子收窄
    15: "...WWWWWWWWWWWWWW...",
    16: "...WWWWWWWWWWWWWW...",
    17: "...WWWWWWWWWWWWWW...",
    18: "...WWWWWWWWWWWWWW...",
    19: "....WWWWWWWWWWWW....",   # 末行收 2px 做圆角；不收会变成硬切平边
}

# ---- 眼睛：2x2 黑豆眼，位置 cols 4-5 / 14-15 ----
EYE_OPEN = {
    7: ".WWWKKWWWWWWWWKKWWW.",
    8: ".WWWKKWWWWWWWWKKWWW.",
}
EYE_BLINK = {                    # 闭眼：只留下缘，压成一条线
    7: ".WWWWWWWWWWWWWWWWWW.",
    8: ".WWWKKWWWWWWWWKKWWW.",
}
EYE_SQUINT = {                   # 眯眼盯着屏幕
    7: ".WWWKKWWWWWWWWKKWWW.",
    8: ".WWWWWWWWWWWWWWWWWW.",
}
EYE_HAPPY = {                    # 笑眼：下缘两点向外撇，读作 "^ ^" 的弯
    7: ".WWWWWWWWWWWWWWWWWW.",
    8: ".WWWKKWWWWWWWWKKWWW.",
}

# ---- 口鼻 ----
MOUTH_NORMAL = {
    10: ".WWWWWWWWPPWWWWWWWW.",   # 小鼻子（2px 是镜像对称的最小单位）
    11: ".WWWWWWWWKKWWWWWWWW.",   # 一字嘴，收窄到 2px，太宽会像涂了口红
}
MOUTH_HAPPY = {                  # 张口笑：鼻子下面接红色小口
    10: ".WWWWWWWWPPWWWWWWWW.",
    11: ".WWWWWWWPPPPWWWWWWW.",
}

# ---- 笔记本：工作状态在身前，盖住身体下半 ----
LAPTOP = {
    17: "...BBBBBBBBBBBBBB...",   # 屏
    18: "...BBBBBBBBBBBBBB...",
    19: "...BBBBBBBBBBBBBB...",   # 底座与屏同宽，别超出身体轮廓
}


def compose(ears, eyes, mouth, extra=None):
    rows = dict(BODY)
    rows.update(eyes)
    rows.update(mouth)
    if extra:
        rows.update(extra)
    return list(ears) + [rows[i] for i in range(4, SIZE)]


FRAMES = {
    # ---- 待机：安静眨眼，1600ms 一换 ----
    "idle": [
        compose(EAR_TALL, EYE_OPEN,  MOUTH_NORMAL),
        compose(EAR_TALL, EYE_BLINK, MOUTH_NORMAL),
    ],
    # ---- 工作中：眯眼 + 身前笔记本，屏幕亮度闪一下（两帧同图，靠定时器切）----
    "working": [
        compose(EAR_TALL, EYE_SQUINT, MOUTH_NORMAL, LAPTOP),
        compose(EAR_TALL, EYE_SQUINT, MOUTH_NORMAL, LAPTOP),
    ],
    # ---- 完成：笑眼 + 张口，500ms 一换（嘴一开一合）----
    "done": [
        compose(EAR_TALL, EYE_HAPPY, MOUTH_HAPPY),
        compose(EAR_TALL, EYE_HAPPY, MOUTH_NORMAL),
    ],
    # ---- 自动化：耳朵压扁，700ms 一换 ----
    "auto": [
        compose(EAR_TALL, EYE_OPEN, MOUTH_NORMAL),
        compose(EAR_FLAT, EYE_OPEN, MOUTH_NORMAL),
    ],
}

ORDER = ["idle", "working", "done", "auto"]


def validate(name, frame):
    if len(frame) != SIZE:
        raise ValueError("%s: 行数 %d != %d" % (name, len(frame), SIZE))
    for i, row in enumerate(frame):
        if len(row) != SIZE:
            raise ValueError("%s 第 %d 行长度 %d != %d: %r"
                             % (name, i, len(row), SIZE, row))
        for ch in row:
            if ch not in CHAR_MAP:
                raise ValueError("%s 第 %d 行有非法字符 %r" % (name, i, ch))
        if row != row[::-1]:
            raise ValueError("%s 第 %d 行不对称: %r" % (name, i, row))


def expand(frame):
    """20x20 逻辑图 -> 60x60 ARGB8888 字节流。

    LVGL 的 ARGB8888 在内存里是 B,G,R,A 顺序（小端下等价于 0xAARRGGBB 的
    32 位值按字节倒序落盘）。这里直接按 B,G,R,A 逐字节写，不依赖任何端序假设。
    """
    out = bytearray()
    for row in frame:
        # 先把一行逻辑像素展开成 PIX 倍宽的字节串
        line = bytearray()
        for ch in row:
            idx = CHAR_MAP[ch]
            if idx < 0:
                b, g, r, a = 0, 0, 0, 0          # 完全透明
            else:
                r, g, b = PALETTE[idx]
                a = 0xFF
            line += bytes((b, g, r, a)) * PIX
        out += line * PIX                        # 纵向重复 PIX 行
    assert len(out) == DISP * DISP * 4, len(out)
    return bytes(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    dst = os.path.join(here, '..', 'firmware', 'main', 'pet_frames.c')

    L = []
    L.append('// 像素宠物素材 —— 由 tools/gen_pet.py 生成，请勿手改')
    L.append('//')
    L.append('// 每帧 %dx%d ARGB8888，内存序 B,G,R,A；透明像素 alpha=0。' % (DISP, DISP))
    L.append('// 尺寸即最终上屏尺寸（屏 240 的一半），运行时不做缩放。')
    L.append('// 帧表排布：pet_frames[state * PET_FRAMES_EACH + frame]')
    L.append('')
    L.append('#include "pet.h"')
    L.append('')

    for state in ORDER:
        for fi, frame in enumerate(FRAMES[state]):
            validate("%s_%d" % (state, fi), frame)
            data = expand(frame)
            L.append('// %s 帧 %d' % (state, fi))
            L.append('static const uint8_t pv_%s_%d[PET_DISP * PET_DISP * 4] = {'
                     % (state, fi))
            for i in range(0, len(data), 16):
                chunk = data[i:i + 16]
                L.append('    ' + ', '.join('0x%02X' % b for b in chunk) + ',')
            L.append('};')
            L.append('')

    L.append('// ---- 帧描述符 ----')
    for state in ORDER:
        for fi in range(2):
            L.append('static const lv_image_dsc_t pe_%s_%d = {' % (state, fi))
            L.append('    .header.magic  = LV_IMAGE_HEADER_MAGIC,')
            L.append('    .header.cf     = LV_COLOR_FORMAT_ARGB8888,')
            L.append('    .header.flags  = 0,')
            L.append('    .header.w      = PET_DISP,')
            L.append('    .header.h      = PET_DISP,')
            L.append('    .header.stride = PET_DISP * 4,')
            L.append('    .data_size     = PET_DISP * PET_DISP * 4,')
            L.append('    .data          = pv_%s_%d,' % (state, fi))
            L.append('};')
            L.append('')

    L.append('const lv_image_dsc_t pet_frames[PET_FRAME_COUNT] = {')
    for state in ORDER:
        for fi in range(2):
            L.append('    pe_%s_%d,' % (state, fi))
    L.append('};')
    L.append('')

    text = '\n'.join(L)
    with io.open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)

    total = len(ORDER) * 2
    print('已生成 %s' % os.path.normpath(dst))
    print('  %d 帧，素材 %dx%d ARGB8888（1:1 上屏），共 %d 字节'
          % (total, DISP, DISP, total * DISP * DISP * 4))


if __name__ == '__main__':
    main()
