# -*- coding: utf-8 -*-
"""对比不同透明度 + 面罩提亮方案的屏显效果。

目的：30% 下白头盔上屏只有 #4B4D52（2.37:1），面部轮廓丢失。
本脚本用真实 BGRA 数据 + LVGL 合成规则，并把面罩提亮，看哪个方案最好。
"""
import io
import os
import re

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'firmware', 'main', 'pet_frames.c')
IMG_W, IMG_H = 120, 96
BG = (0x06, 0x08, 0x0C)


def parse_first_frame():
    txt = io.open(SRC, 'r', encoding='utf-8').read()
    m = re.search(r'static const uint8_t pv_idle_0\[(\d+)\]\s*=\s*\{(.*?)\};',
                  txt, re.S)
    vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(2))]
    return vals


def to_rgba(vals):
    img = Image.new('RGBA', (IMG_W, IMG_H))
    px = img.load()
    for y in range(IMG_H):
        for x in range(IMG_W):
            o = (y * IMG_W + x) * 4
            b, g, r, a = vals[o], vals[o + 1], vals[o + 2], vals[o + 3]
            px[x, y] = (r, g, b, a)
    return img


def lift_face(img, k_old, k_new, w_new=None):
    """把面罩主色从 k_old 换成 k_new。

    踩过：这里曾把 k_old 硬编码成 0x13151B，后来 gen_pet.py 把面罩提亮到了
    0x1C212C，这个函数就一行都改不到了 —— 但它不报错，C/D 两个方案
    悄悄退化成和 B 完全一样，看图时会误以为"提亮面罩没效果"。
    所以 k_old 必须从调用方传进来，并且下面加了命中数断言。
    """
    out = img.copy()
    px = out.load()
    hit = 0
    for y in range(IMG_H):
        for x in range(IMG_W):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            if (r, g, b) == k_old:
                px[x, y] = (k_new[0], k_new[1], k_new[2], a)
                hit += 1
            elif w_new and (r, g, b) == (0xEC, 0xF1, 0xF8):
                px[x, y] = (w_new[0], w_new[1], w_new[2], a)
    assert hit > 0, 'lift_face 没命中任何像素：k_old=%r 与调色板不一致' % (k_old,)
    return out


def comp(img, opa):
    out = Image.new('RGB', img.size, BG)
    op, ip = out.load(), img.load()
    for y in range(IMG_H):
        for x in range(IMG_W):
            r, g, b, a = ip[x, y]
            if a == 0:
                continue
            af = (a / 255.0) * (opa / 255.0)
            op[x, y] = tuple(int(round(f * af + bk * (1 - af)))
                             for f, bk in zip((r, g, b), BG))
    return out


def main():
    vals = parse_first_frame()
    base = to_rgba(vals)

    # 面罩主色必须从调色板取，不能写死 —— 见 lift_face 的注释
    K_OLD = (0x1C, 0x21, 0x2C)      # gen_pet.py PAL['K']
    K_NEW = (0x26, 0x2D, 0x3C)      # 再往上一档，用于对比

    cases = [
        ('A 30% 原色', base, 76),
        ('B 40% 原色(当前)', base, 102),
        ('C 40%+面罩再亮', lift_face(base, K_OLD, K_NEW), 102),
        ('D 45% 原色', base, 115),
    ]

    Z = 3
    cw, ch = IMG_W * Z, IMG_H * Z
    PAD = 20
    W = cw * len(cases) + PAD * (len(cases) + 1)
    H = ch + PAD * 2
    sheet = Image.new('RGB', (W, H), (10, 11, 14))
    for i, (label, img, opa) in enumerate(cases):
        cell = comp(img, opa).resize((cw, ch), Image.NEAREST)
        sheet.paste(cell, (PAD + i * (cw + PAD), PAD))
    p = os.path.join(HERE, 'pet_opa_options.png')
    sheet.save(p)
    print('写出', p, sheet.size)
    print('从左到右:', ' | '.join(c[0] for c in cases))


if __name__ == '__main__':
    main()
