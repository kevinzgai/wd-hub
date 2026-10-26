#!/usr/bin/env python3
"""
点阵字体生成器 —— 把 Windows 系统字体烘焙成 C 头文件，供 ESP32 直接绘制。

输出:
    firmware/main/font_ascii.h   8x16 ASCII  (0x20-0x7E, 95 个字符)
    firmware/main/font_cjk.h     16x16 汉字  (只包含 CHARSET 里用到的字)

用法:
    python tools/gen_font.py

改显示内容后，把新字加进 CHARSET 再跑一次即可。
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "firmware", "main")

FONT_CJK = r"C:\Windows\Fonts\simhei.ttf"      # 黑体，点阵化清晰
FONT_ASCII = r"C:\Windows\Fonts\consola.ttf"   # 等宽，天然贴合 8x16 网格
FONT_BIG = r"C:\Windows\Fonts\arialbd.ttf"     # 粗体数字，做时钟

# 需要烘焙的汉字/符号。服务端 JSON 里出现的中文字都得在这里。
CHARSET = (
    "你好小智周一二三四五六日"
    "设备数据看板运行服务拉取刷新每两次秒"
    "主机内存温度占用连接中已失败等待网络"
    "未就绪请检查路由信号强度在线离线"
    "潮光影·：，。！？、（）【】—"
)

COLOR_KEY = (0, 0, 0)  # 透明背景，用调色板键色处理

ASCII_W, ASCII_H = 8, 16
CJK_W, CJK_H = 16, 16

ASCII_SIZE = 16
CJK_SIZE = 15


def render(ch: str, font_path: str, size: int, box_w: int, box_h: int):
    """把单个字符渲染成黑白点阵，按实际墨迹包围盒精确居中。

    先在足够大的画布上渲染，裁出墨迹，再缩放居中贴进目标框 —— 规避不同字体
    的 ascent/descent 度量差异导致的偏移。
    """
    pad = 8
    cw, chh = box_w + pad * 2, box_h + pad * 2
    font = ImageFont.truetype(font_path, size)

    img = Image.new("1", (cw, chh), 0)
    draw = ImageDraw.Draw(img)
    draw.text((pad, pad), ch, fill=1, font=font)

    bbox = img.getbbox()  # 实际墨迹范围
    if not bbox:
        return [0] * box_h  # 空白字符（空格）

    ink = img.crop(bbox)
    iw, ih = ink.size

    # 太高太宽就等比缩到框内（留 1px 余量）
    max_w, max_h = box_w, box_h - 1
    if iw > max_w or ih > max_h:
        scale = min(max_w / iw, max_h / ih)
        ink = ink.resize((max(1, int(iw * scale)), max(1, int(ih * scale))), Image.LANCZOS)
        iw, ih = ink.size

    canvas = Image.new("1", (box_w, box_h), 0)
    canvas.paste(ink, ((box_w - iw) // 2, (box_h - ih) // 2))

    rows = []
    for py in range(box_h):
        bits = 0
        for px in range(box_w):
            if canvas.getpixel((px, py)):
                bits |= 1 << (box_w - 1 - px)
        rows.append(bits)
    return rows


def to_c_array(rows, width):
    """每 8 位一个字节，高字节在前。"""
    out = []
    nbytes = (width + 7) // 8
    for bits in rows:
        for i in range(nbytes):
            out.append((bits >> (8 * (nbytes - 1 - i))) & 0xFF)
    return out


def gen_ascii():
    lines = [
        "// 自动生成，勿手改 —— tools/gen_font.py",
        "// 8x16 ASCII 点阵，索引 = 字符码 - 0x20",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define FONT_ASCII_W  {ASCII_W}",
        f"#define FONT_ASCII_H  {ASCII_H}",
        f"#define FONT_ASCII_FIRST 0x20",
        f"#define FONT_ASCII_LAST  0x7E",
        "",
        "static const uint8_t font_ascii_8x16[][16] = {",
    ]
    for code in range(0x20, 0x7F):
        ch = chr(code)
        rows = render(ch, FONT_ASCII, ASCII_SIZE, ASCII_W, ASCII_H)
        data = to_c_array(rows, ASCII_W)
        hexs = ",".join(f"0x{b:02X}" for b in data)
        label = ch if ch not in ("\\",) else "\\\\"
        lines.append(f"    {{{hexs}}},  // 0x{code:02X} '{label}'")
    lines.append("};")
    return "\n".join(lines) + "\n"


def gen_cjk():
    chars = sorted({c for c in CHARSET if ord(c) > 0x7E})
    lines = [
        "// 自动生成，勿手改 —— tools/gen_font.py",
        "// 16x16 汉字点阵，按 codepoint 升序排列，二分查找",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define FONT_CJK_W  {CJK_W}",
        f"#define FONT_CJK_H  {CJK_H}",
        "",
        "typedef struct {",
        "    uint16_t cp;          // Unicode 码点",
        "    uint8_t  bmp[32];     // 16x16, 每行 2 字节",
        "} cjk_glyph_t;",
        "",
        f"#define FONT_CJK_COUNT {len(chars)}",
        "",
        "static const cjk_glyph_t font_cjk_16x16[] = {",
    ]
    for ch in chars:
        rows = render(ch, FONT_CJK, CJK_SIZE, CJK_W, CJK_H)
        data = to_c_array(rows, CJK_W)
        hexs = ",".join(f"0x{b:02X}" for b in data)
        lines.append(f"    {{0x{ord(ch):04X}, {{{hexs}}}}},  // {ch}")
    lines.append("};")
    return "\n".join(lines) + "\n"


BIG_W, BIG_H = 28, 40
BIG_CHARS = "0123456789:"
BIG_SIZE = 56


def gen_big():
    """大号数字，用于屏幕顶部时钟。4 字节/行（28 位用 32 位存）。"""
    nbytes = (BIG_W + 7) // 8  # 4
    lines = [
        "// 自动生成，勿手改 —— tools/gen_font.py",
        "// 28x40 数字+冒号，索引见 font_big_index()",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define FONT_BIG_W  {BIG_W}",
        f"#define FONT_BIG_H  {BIG_H}",
        f"#define FONT_BIG_ROWBYTES {nbytes}",
        "",
        f'static const char font_big_charset[] = "{BIG_CHARS}";',
        f"#define FONT_BIG_COUNT {len(BIG_CHARS)}",
        "",
        f"static const uint8_t font_big_28x40[][{BIG_H * nbytes}] = {{",
    ]
    for ch in BIG_CHARS:
        rows = render(ch, FONT_BIG, BIG_SIZE, BIG_W, BIG_H)
        out = []
        for bits in rows:
            for i in range(nbytes):
                out.append((bits >> (8 * (nbytes - 1 - i))) & 0xFF)
        hexs = ",".join(f"0x{b:02X}" for b in out)
        lines.append(f"    {{{hexs}}},  // '{ch}'")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    for fp in (FONT_ASCII, FONT_CJK):
        if not os.path.exists(fp):
            sys.exit(f"找不到字体: {fp}")

    os.makedirs(OUT_DIR, exist_ok=True)

    p1 = os.path.join(OUT_DIR, "font_ascii.h")
    with open(p1, "w", encoding="utf-8", newline="\n") as f:
        f.write(gen_ascii())

    p2 = os.path.join(OUT_DIR, "font_cjk.h")
    with open(p2, "w", encoding="utf-8", newline="\n") as f:
        f.write(gen_cjk())

    p3 = os.path.join(OUT_DIR, "font_big.h")
    with open(p3, "w", encoding="utf-8", newline="\n") as f:
        f.write(gen_big())

    n_cjk = len({c for c in CHARSET if ord(c) > 0x7E})
    print(f"OK  {p1}")
    print(f"OK  {p2}")
    print(f"OK  {p3}")
    print(f"    ASCII 95 字符, 中文 {n_cjk} 字, 大号数字 {len(BIG_CHARS)} 个")


if __name__ == "__main__":
    main()
