# -*- coding: utf-8 -*-
"""
从 pet_frames.c 解析真实像素，按屏上合成规则渲染模拟图。

屏上真实规则（LVGL）：
  dst = #06080C (COL_BG)
  像素 = ARGB8888，内存字节序 B,G,R,A
  lv_obj_set_style_image_opa(PET_OPA=76) 会把像素 alpha 再乘 76/255
  最终 out = src*a + dst*(1-a)，a = pix_a/255 * 76/255

输出：
  tools/pet_screen_mock.png   —— 240x240 真实尺寸屏显（宠物在右下 (4,4)）
  tools/pet_compare.png       —— 左原件 / 右30%透明的对比，3倍放大
"""
import io
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'firmware', 'main', 'pet_frames.c')
OUT_DIR = HERE

SCR_W = SCR_H = 240
BG = (0x06, 0x08, 0x0C)
PET_X, PET_Y = 4, 4           # main.cpp 里的 PET_X / PET_Y
IMG_W, IMG_H = 120, 96        # 素材实际尺寸（描述符 .w/.h，没有留白）
OPA = 102                     # main.cpp 里的 PET_OPA = LV_OPA_40

# LVGL 的 LV_ALIGN_BOTTOM_RIGHT 是"对象右下角对齐父右下角"，再按 ofs 偏移。
# 踩过：第一版直接 canvas.paste(comp, (PET_X, PAD_Y)) 把它贴到了左上角，
# 拿这张图去判断屏上观感，位置全是错的。改为按 BOTTOM_RIGHT 反推左上角。
PET_LEFT = SCR_W - IMG_W + PET_X
PET_TOP = SCR_H - IMG_H + PET_Y


def parse_frames():
    """按**描述符表**还原 8 帧，而不是数 static 数组。

    踩过：去重之后 pet_frames.c 里只有 5 个 static 数组（idle_0 被 working_2
    复用等），如果按数组逐个解析就只有 5 张，画出来的"8 帧总览"缺帧且错位。
    必须读 pet_frames[] 里 .data 指向的数组名，才能拿到 8 个正确顺序的帧。
    """
    txt = io.open(SRC, 'r', encoding='utf-8').read()

    # 先把所有 static 数组解析成 name -> bytes
    blobs = {}
    for m in re.finditer(
        r'static const uint8_t (pv_\w+)\[(\d+)\]\s*=\s*\{(.*?)\};', txt, re.S
    ):
        name, size, body = m.group(1), int(m.group(2)), m.group(3)
        vals = re.findall(r'0x([0-9A-Fa-f]{2})', body)
        data = bytes(int(v, 16) for v in vals)
        assert len(data) == size, '%s: 期望 %d 实得 %d' % (name, size, len(data))
        blobs[name] = data

    # 再按描述符表顺序取出 8 帧
    frames = []
    tbl = re.search(
        r'const lv_image_dsc_t pet_frames\[PET_FRAME_COUNT\]\s*=\s*\{(.*?)\n\};',
        txt, re.S)
    assert tbl, '找不到 pet_frames[] 描述符表'
    for m in re.finditer(r'\.data\s*=\s*(pv_\w+)', tbl.group(1)):
        nm = m.group(1)
        assert nm in blobs, '描述符引用了不存在的数组 %s' % nm
        frames.append((nm, blobs[nm]))

    return frames


def to_rgba(data, w, h):
    """内存序 B,G,R,A -> PIL RGBA。"""
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            b, g, r, a = data[o], data[o + 1], data[o + 2], data[o + 3]
            px[x, y] = (r, g, b, a)
    return img


def composite(img, bg, opa):
    """按 LVGL 规则合成到纯色背景上。"""
    out = Image.new('RGB', img.size, bg)
    op = out.load()
    ip = img.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = ip[x, y]
            if a == 0:
                continue
            # 两段 alpha 相乘：像素自带 alpha * 对象透明度
            af = (a / 255.0) * (opa / 255.0)
            br, bgc, bb = bg
            op[x, y] = (
                int(round(r * af + br * (1 - af))),
                int(round(g * af + bgc * (1 - af))),
                int(round(b * af + bb * (1 - af))),
            )
    return out


def main():
    frames = parse_frames()
    print('解析到 %d 帧' % len(frames))

    # ---- 1. 真实屏显模拟：8 帧各出一张 240x240，再拼成 4x2 总览 ----
    scr_cells = []
    for name, data in frames:
        img = to_rgba(data, IMG_W, IMG_H)
        canvas = Image.new('RGB', (SCR_W, SCR_H), BG)
        # BOTTOM_RIGHT 对齐后的真实左上角，超出部分由 PIL 自动裁掉
        comp = composite(img, BG, OPA)
        canvas.paste(comp, (PET_LEFT, PET_TOP))
        scr_cells.append((name, canvas))

    Z = 2  # 总览放大倍数（240*2=480，4 列就是 1920，合适）
    gw, gh = SCR_W * Z, SCR_H * Z
    sheet = Image.new('RGB', (gw * 4, gh * 2), (18, 20, 26))
    for i, (name, c) in enumerate(scr_cells):
        cx, cy = (i % 4) * gw, (i // 4) * gh
        sheet.paste(c.resize((gw, gh), Image.NEAREST), (cx, cy))
    p1 = os.path.join(OUT_DIR, 'pet_screen_mock.png')
    sheet.save(p1)
    print('写出', p1, sheet.size)

    # ---- 2. 对比图：原件 vs 30% 透明，各 3 倍放大 ----
    Z2 = 3
    cw, ch = IMG_W * Z2, IMG_H * Z2
    PADX = 24
    cmp_img = Image.new('RGB', (cw * 2 + PADX * 3, ch + PADX * 3 + 40), (14, 16, 20))
    name, data = frames[0]
    img = to_rgba(data, IMG_W, IMG_H)
    # 左：原件叠在深灰上（看清像素本身）
    left = composite(img, (32, 36, 44), 255).resize((cw, ch), Image.NEAREST)
    cmp_img.paste(left, (PADX, PADX + 40))
    # 右：30% 透明叠在真实屏底色上
    right = composite(img, BG, OPA).resize((cw, ch), Image.NEAREST)
    cmp_img.paste(right, (PADX * 2 + cw, PADX + 40))
    p2 = os.path.join(OUT_DIR, 'pet_compare.png')
    cmp_img.save(p2)
    print('写出', p2, cmp_img.size)


if __name__ == '__main__':
    main()
