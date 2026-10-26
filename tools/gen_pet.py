#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成像素宠物的 C 素材（firmware/main/pet_frames.c）。

设计依据（用户提供的参考图：猫耳机器人 + 圆黑面罩 + 青色眼镜 + 两侧耳机）
------------------------------------------------------------------------
参考图是 556x556 的完整角色，暗部占 70%（面罩 #1A191C / 马甲 #121214 /
裤子 #1D1E21），而屏底色是 #06080C、宠物又以 LV_OPA_30 半透明压在最底层。
直接把整图缩到 120px 后，暗部与背景几乎同色 —— 实测只能看出"一坨黑的"，
猫耳和眼镜之外全部消失。

所以只取**头部**，并手绘成 40x32 的像素画：
  - 头部是唯一有高对比结构的区域（白头盔 + 青眼镜 + 深面罩）
  - 手绘而不是自动量化，因为自动量化在 40 列下会在面罩里留下反光噪点
    （原图面罩是玻璃质感，渐变被压成零散亮点）
  - 40x32 -> 120x96，横向 3 倍整数放大，纵向居中留 12px 上下留白

为什么是 40 列而不是 24
------------------------
24x24 试过：耳机、眼镜、猫耳三者放不下，画出来是个"通用机器人"，
丢掉了参考图的辨识度。40 列刚好能同时容纳猫耳轮廓 + 椭圆面罩 +
两条 2px 青眼镜 + 两侧耳机。

运行时零缩放
------------
素材就是 120x120，1:1 上屏。不用 lv_image_set_scale —— 那个函数只改
"绘制变换"不改对象 w/h，pivot 默认在中心，放大后会以中心向四周溢出、
超出对象边界被裁掉（踩过，屏幕只露左上角）。

8 帧 x 120x96 x 4B(ARGB8888) = 368640B（上下各留 12px 透明凑满 120x120 画布）

换帧差异
--------
只改眼睛。青色眼镜是 30% 透明度下唯一还能看清的元素，用它承载状态：
    idle    睁眼 -> 闭眼（慢眨眼）
    working 睁眼 -> 睁眼（高频抖动，配合更短的周期）
    done    弯眼 ^ -> 弯眼 ^^（笑）
    auto    睁眼 -> 眼睛变窄（等待你操作）
"""
import io
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'main', 'pet_frames.c')

# 逻辑画布 40x32，放大 3 倍 -> 120x96，纵向居中到 120
N, H, PIX = 40, 32, 3
DISP = N * PIX               # 120

# 调色板。'.' 是透明，其余对齐屏底色 #06080C 之后的可辨识亮度。
#
# 为什么面罩 K 不是参考图的 #1A191C：
# --------------------------------
# 屏底色 #06080C 已经极暗，K 若照抄参考图（#1A191C），在 PET_OPA_40 下
# 上屏只剩 #0F1115，和背景的对比度 1.15:1 —— 面罩和背景直接融成一片，
# 头部看起来就是个"白圈里挖了个洞"，分不出脸。
# 提到 #1C212C 后上屏 #14171E，对比度约 1.5:1，能看出一个独立的深色椭圆。
# 这是"可辨识"与"别太亮抢文字"之间的折中值，不要随意调回。
PAL = {
    '.': (0x00, 0x00, 0x00, 0),        # 透明
    'W': (0xEC, 0xF1, 0xF8, 0xFF),     # 头盔亮白
    'w': (0xB4, 0xBD, 0xCB, 0xFF),     # 头盔暗侧
    'K': (0x1C, 0x21, 0x2C, 0xFF),     # 面罩主体（比参考图亮一档，见上）
    'k': (0x32, 0x3A, 0x4A, 0xFF),     # 面罩高光/镜梁
    'H': (0x6E, 0x78, 0x88, 0xFF),     # 耳机
    'h': (0x44, 0x4B, 0x56, 0xFF),     # 耳机外沿
    'C': (0x35, 0xE8, 0xF5, 0xFF),     # 眼镜亮
    'c': (0x1C, 0x91, 0x9E, 0xFF),     # 眼镜暗（上下端）
    'E': (0x6B, 0x74, 0x83, 0xFF),     # 内耳
}

def blank():
    return [['.'] * N for _ in range(H)]

def ell(cx, cy, rx, ry, x, y):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2

def compose(eyes):
    """eyes: 'open' | 'blink' | 'happy' | 'narrow'

    只有眼睛部分随状态变化，其余（猫耳/头盔/面罩/耳机）完全一致，
    这样换帧时不会出现"整个头在抖"的廉价感。
    """
    g = blank()

    # ---- 猫耳：逐行区间表，避免用取整造成锯齿 ----
    EAR_L = {
        1: (8, 11), 2: (8, 11), 3: (7, 12), 4: (7, 12),
        5: (6, 12), 6: (6, 12), 7: (5, 12), 8: (5, 12),
        9: (5, 12), 10: (5, 12), 11: (5, 12),
    }
    for y, (a, b) in EAR_L.items():
        for x in range(a, b + 1):
            g[y][x] = 'W'
        for x in range(a + 1, b):
            g[y][x] = 'E'
    for y in range(1, 12):                      # 右耳镜像
        for x in range(N // 2):
            g[y][N - 1 - x] = g[y][x]

    # ---- 头盔 ----
    for y in range(H):
        for x in range(N):
            d = ell(19.5, 17.5, 13.8, 13.2, x, y)
            if d <= 1.0:
                g[y][x] = 'w' if d > 0.84 else 'W'

    # ---- 耳机：贴住头两侧 ----
    for y in range(15, 28):
        for x in range(2, 7):
            if ell(19.5, 21, 15.4, 8.4, x, y) <= 1.0:
                g[y][x] = 'h' if x == 2 else 'H'
        for x in range(33, 38):
            if ell(19.5, 21, 15.4, 8.4, x, y) <= 1.0:
                g[y][x] = 'h' if x == 37 else 'H'

    # ---- 面罩 ----
    for y in range(H):
        for x in range(N):
            d = ell(19.5, 19.5, 11.4, 8.8, x, y)
            if d <= 1.0:
                g[y][x] = 'k' if d > 0.88 else 'K'

    # ---- 眼睛 ----
    # 镜梁（y=19 的 'k' 横线）不在这里画，统一放到函数末尾 —— 见下方注释。
    if eyes == 'open':
        for y in range(16, 24):
            for x in (13, 14):
                g[y][x] = 'C' if 17 <= y <= 22 else 'c'
            for x in (25, 26):
                g[y][x] = 'C' if 17 <= y <= 22 else 'c'
    elif eyes == 'blink':
        # 闭眼：只留一条 1px 的暗青横线
        for x in range(13, 15):
            g[20][x] = 'c'
        for x in range(25, 27):
            g[20][x] = 'c'
    elif eyes == 'narrow':
        # 变窄：中间几行，表示"在等你"
        for y in range(18, 23):
            for x in (13, 14):
                g[y][x] = 'C'
            for x in (25, 26):
                g[y][x] = 'C'
    elif eyes == 'happy':
        # 笑眼 ^：每只眼画成 3 列阶梯，中间列最高，两侧各降 1 行。
        # 左右必须严格镜像，否则 validate() 会直接报错。
        #   左眼中心 x=13, 右眼中心 x=26（相对 19.5 对称）
        for cx in (13, 26):
            g[18][cx]   = 'C'
            g[19][cx-1] = 'C'; g[19][cx+1] = 'C'
            g[20][cx-2] = 'C'; g[20][cx+2] = 'C'
    elif eyes == 'happy_small':
        # 笑收：弧高只有 happy 的一半（2 行阶梯而不是 3 行）。
        # 和 happy 交替形成"笑开 -> 笑收"，避免两帧一模一样导致动画静止。
        # 注意阶梯宽度也随之收窄，否则同高度的横线会和 happy 撞脸。
        for cx in (13, 26):
            g[19][cx]   = 'C'
            g[20][cx-1] = 'C'; g[20][cx+1] = 'C'
    else:
        raise ValueError(eyes)

    # 镜梁 y=19 x=15..24（面罩高光 'k'，横贯两眼之间）。
    # 必须在眼睛之后统一补，不能只写在 open/blink/narrow 三个分支里：
    # happy 分支的弧线会覆盖 y=19 的中间段，漏补的话面罩中央少一条亮边，
    # 上屏表现为"笑的那一帧脸突然暗了一块"，两帧交替时就是闪烁。
    # 放在这里而不是各分支里，也保证了四种表情的镜梁绝对一致。
    for x in range(15, 25):
        g[19][x] = 'k'

    return g

# 8 帧：4 态 x 2 帧
#
# 每种状态的两帧必须是**不同画面**，否则定时器换帧等于没换，动画是死的。
# 踩过：done 原本写 ('happy','happy')，两帧字节完全相同，
# "笑口一开一合"变成了静止画面；直接白占 46KB 且肉眼完全无变化。
# 现在 done 用 happy/happy_small 两档弧高，形成"笑开 -> 笑收"的呼吸感。
FRAMES = [
    ('idle',    'open'),
    ('idle',    'blink'),
    ('working', 'open'),
    ('working', 'narrow'),
    ('done',    'happy'),
    ('done',    'happy_small'),
    ('auto',    'narrow'),
    ('auto',    'blink'),
]

def validate(g, name):
    for y in range(H):
        assert len(g[y]) == N, '%s row %d' % (name, y)
        for ch in g[y]:
            assert ch in PAL, '%s row %d bad char %r' % (name, y, ch)
        # 左右必须严格镜像（画歪了直接报错，别等到烧进板子才发现）
        row = ''.join(g[y])
        if row != row[::-1]:
            raise ValueError('%s 第 %d 行不对称: %r' % (name, y, row))
def expand(g):
    """40x32 -> 120x96 的 BGRA 字节流（纵向 3 倍、横向 3 倍）。"""
    out = bytearray()
    for y in range(H):
        line = bytearray()
        for x in range(N):
            # PAL 里的元组是 (R,G,B,A)。要写成 LVGL ARGB8888 的内存序 B,G,R,A，
            # 所以必须显式重排：bytes((b, g, r, a))。
            # 踩过：这里曾写成 `b, gg, r, a = PAL[...]`，等于把 R 当 B、B 当 R，
            # 青色眼镜 0x35E8F5 上屏后变成黄褐色（R<->B 互换）。
            r, gg, b, a = PAL[g[y][x]]
            line += bytes((b, gg, r, a)) * PIX
        out += line * PIX
    assert len(out) == DISP * (H * PIX) * 4, len(out)
    return bytes(out)

def main():
    parts = []
    parts.append('// 自动生成，请勿手改。改工具后重跑：python tools/gen_pet.py\n')
    parts.append('// 源：40x32 手绘像素画（头部特写），3 倍整数放大到 120x96 后纵向居中。\n')
    parts.append('#include "pet.h"\n\n')

    # ---- 生成所有帧的字节，并做两项检查 ----
    # 1) 同状态的两帧必须不同。否则定时器换帧等于没换，动画是死的。
    #    踩过：done 原本写 ('happy','happy')，两帧字节完全相同，
    #    笑口"一开一合"变成静止画面，肉眼毫无变化。
    # 2) 不同状态之间允许画面相同（如 idle 与 working 都用 open 眼），
    #    但必须**共享同一份静态数组**，否则同样的 46KB 重复存多份。
    #    实测 8 帧里只有 5 张不同画面，去重省 135KB。
    blobs = []          # 每个 FRAMES 项对应的字节
    for idx, (state, eyes) in enumerate(FRAMES):
        g = compose(eyes)
        validate(g, '%s_%d' % (state, idx))
        blobs.append(expand(g))

    for i in range(0, len(FRAMES), 2):
        if blobs[i] == blobs[i + 1]:
            raise ValueError(
                '状态 %s 的两帧画面完全相同（%s / %s），换帧不会有任何变化'
                % (FRAMES[i][0], FRAMES[i][1], FRAMES[i + 1][1]))

    # 按内容去重：相同字节只输出一个 static 数组
    uniq = []           # [(name, data)]
    alias = []          # 每个帧 -> uniq 下标
    by_hash = {}
    for idx, data in enumerate(blobs):
        key = bytes(data)
        if key in by_hash:
            ui = by_hash[key]
            alias.append(ui)
            print('  帧 %d (%s) 与帧 %d 画面相同，复用数组 %s'
                  % (idx, FRAMES[idx][0], ui, uniq[ui][0]))
        else:
            ui = len(uniq)
            by_hash[key] = ui
            nm = 'pv_%s_%d' % (FRAMES[idx][0], idx)
            uniq.append((nm, data))
            alias.append(ui)

    for nm, data in uniq:
        parts.append('static const uint8_t %s[%d] = {\n' % (nm, len(data)))
        # 每行 24 字节，便于 diff
        for i in range(0, len(data), 24):
            chunk = data[i:i + 24]
            parts.append('  ' + ','.join('0x%02X' % b for b in chunk) + ',\n')
        parts.append('};\n\n')

    # 描述符表。注意这里不再输出 PAD_Y —— 素材就是 120x96 本身，
    # 没有留白，pet.h 里也删掉了同名宏（原来那两处都是错的、且从未被引用）。
    parts.append('const lv_image_dsc_t pet_frames[PET_FRAME_COUNT] = {\n')
    for idx in range(len(FRAMES)):
        nm = uniq[alias[idx]][0]
        parts.append('    {\n')
        parts.append('        .header = { .cf = LV_COLOR_FORMAT_ARGB8888, .w = %d, .h = %d, .stride = %d },\n'
                     % (DISP, H * PIX, DISP * 4))
        parts.append('        .data_size = %d,\n' % (DISP * H * PIX * 4))
        parts.append('        .data = %s,\n' % nm)
        parts.append('    },\n')
    parts.append('};\n')

    src = ''.join(parts)
    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(src)
    print('写入 %s  (%.1f KB)' % (OUT, len(src) / 1024.0))
    print('  %d 帧 -> %d 份数组，省 %.0f KB'
          % (len(FRAMES), len(uniq),
             (len(FRAMES) - len(uniq)) * DISP * H * PIX * 4 / 1024.0))
    # 同时输出预览图，方便肉眼核对
    try:
        from PIL import Image
        cell = 130
        sheet = Image.new('RGB', (cell * 4 + 10, cell * 2 + 10), (0x06, 0x08, 0x0C))
        for idx, (state, eyes) in enumerate(FRAMES):
            g = compose(eyes)
            img = Image.new('RGBA', (N, H))
            p = img.load()
            for y in range(H):
                for x in range(N):
                    # PAL 已经是 (R,G,B,A)，和 PIL 的 RGBA 顺序一致，直接赋值。
                    # 踩过：这里曾写成 (c[2],c[1],c[0],c[3])，预览图跟着固件一起错，
                    # 两边"互相印证"，导致 R<->B 互换一直没被发现。
                    p[x, y] = PAL[g[y][x]]
            big = img.resize((DISP, H * PIX), Image.NEAREST)
            r, gg, b, a = big.split()
            # 与 main.cpp 的 PET_OPA 保持一致（LV_OPA_40 = 102/255）。
            # 这里必须跟着改，否则预览图的亮度跟屏上对不上，
            # 就会得出"屏上应该更亮"之类的错误结论。
            a = a.point(lambda v: int(v * 102 / 255.0))
            hero = Image.new('RGBA', (DISP, H * PIX), (0x06, 0x08, 0x0C, 255))
            hero.alpha_composite(Image.merge('RGBA', (r, gg, b, a)), (0, 0))
            sheet.paste(hero.convert('RGB'),
                        ((idx % 4) * cell + 5, (idx // 4) * cell + 5))
        pv = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pet_preview.png')
        sheet.save(pv)
        print('预览 %s' % pv)
    except ImportError:
        pass

if __name__ == '__main__':
    main()
