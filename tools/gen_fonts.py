#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
重新生成固件用的点阵字库（中文 + 符号 合体）。

背景：
  LVGL 的一个 label 只能用一个字体，想在中文旁边显示 ★ ● 这类符号，
  要么做 fallback 链，要么把符号并进同一个字体。这里选后者 —— 用 lv_font_conv
  的多 --font 能力，一次生成同时含中文和符号的字体，label 里直接写就行。

为什么不用彩色 emoji：
  Segoe UI Emoji / Apple Color Emoji 是彩色字体，lv_font_conv 1.5.3 不支持
  彩色字形，栅格化出来只有实心剪影（实测是黑块），没法用。

字号与字符集是**分开**的：
  28px 只显示时钟和两句提示，喂它 4000 字纯属浪费（三个字库里它最贵）。
  12/16px 要显示服务端下发的会话标题，必须留足余量，否则满屏方格。
  字符集由 tools/gen_charset.py 生成两份：charset_ui.txt / charset.txt。

用法：
  python tools/gen_charset.py     # 先更新字符集
  python tools/gen_fonts.py       # 再生成字库
"""

import os
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT      = os.path.dirname(TOOLS_DIR)
MAIN_DIR  = os.path.join(ROOT, "firmware", "main")

# 界面里当图标用的几何符号
EXTRA_SYMBOLS = "●○◆★☆▲△▼■□✓✔✗⚡♥♪☀⏱⏰⚙▶⏸☰✈↔→↑↓"

# ---------------------------------------------------------------------------
# 字体探测：按平台找最接近"黑体"的无衬线中文字体
#
# 原脚本硬编码 C:/Windows/Fonts/simhei.ttf，换到 Mac 直接跑不了。
# 实测踩坑：macOS 的 Hei.ttf（SIL-Hei）虽然形似黑体，但缺少 OS/2 表，
# lv_font_conv 会报 "os/2 table not found"；Arial Unicode.ttf 可用。
# ---------------------------------------------------------------------------
CJK_CANDIDATES = [
    os.environ.get("LV_FONT_CJK", ""),
    "C:/Windows/Fonts/simhei.ttf",                                        # Windows 黑体
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",               # macOS（含完整 CJK）
    "/Library/Fonts/Arial Unicode.ttf",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttf",             # Linux
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]

SYM_CANDIDATES = [
    os.environ.get("LV_FONT_SYM", ""),
    "C:/Windows/Fonts/seguisym.ttf",                                      # Windows Segoe UI Symbol
    "/System/Library/Fonts/Apple Symbols.ttf",                            # macOS
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",                    # Linux
]

# (字号, 变量名, 字符集文件, 说明)
#
# 28px 只给 UI 文案：它只渲染时钟、数字、"WiFi 未连接"、"配网模式"。
# 复用全量字符集会让它从 ~40KB 涨到 214KB，纯浪费 Flash。
SIZES = [
    (12, "font_cn_12", "charset.txt",    "列表/元信息，含 GB2312 一级常用字"),
    (16, "font_cn_16", "charset.txt",    "会话标题/行标题，含 GB2312 一级常用字"),
    (28, "font_cn_28", "charset_ui.txt", "时钟/大标题，只需 UI 文案"),
]


def pick(candidates, what):
    for p in candidates:
        if p and os.path.exists(p):
            return p
    sys.exit(
        f"找不到可用的{what}字体。可用环境变量指定：\n"
        + "\n".join(f"  {c or '(未设置)'}" for c in candidates)
    )


def main():
    cjk = pick(CJK_CANDIDATES, "中文")
    sym = pick(SYM_CANDIDATES, "符号")
    print(f"中文字体: {cjk}")
    print(f"符号字体: {sym}")

    entry = os.path.join(TOOLS_DIR, "node_modules", "lv_font_conv", "lv_font_conv.js")
    if not os.path.exists(entry):
        sys.exit(f"找不到 lv_font_conv: {entry}\n先跑: cd tools && npm install "
                 f"--registry=https://registry.npmmirror.com")

    total = 0
    for size, name, charset_file, note in SIZES:
        path = os.path.join(TOOLS_DIR, charset_file)
        if not os.path.exists(path):
            sys.exit(f"找不到字符集 {path}，先跑 tools/gen_charset.py")

        with open(path, encoding="utf-8") as f:
            charset = f.read().strip()
        # 界面里用作分隔符的标点，源码里不一定出现，单独补
        charset += "·—…「」"

        out = os.path.join(MAIN_DIR, f"{name}.c")
        cmd = [
            "node", entry,
            "--font", cjk, "--symbols", charset,
            # 关键：--symbols 走字符串参数，空格会被命令行吞掉，结果字库
            # ASCII 从 33(!) 开始，U+0020 缺失。界面里 "● 连接中" 这种带空格
            # 的串，空格就会渲染成占位方框。用 --range 显式包进 0x20~0x7E。
            "--range", "0x20-0x7E",
            "--font", sym, "--symbols", EXTRA_SYMBOLS,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            # 关键：必须关掉位图压缩。压缩后 bitmap_format=1，需要
            # LV_USE_FONT_COMPRESSED 打开才能解压，否则 LVGL 把所有字
            # 都画成占位方框（中文、数字、符号一起变方格）。
            "--no-compress",
            "--lv-include", "lvgl.h",
            "--lv-font-name", name,
            "-o", out,
        ]
        print(f"[{size:2d}px] {name:12s} {len(charset):5d} 字  {note} ...",
              end="", flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(" 失败")
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            sys.exit(1)
        kb = os.path.getsize(out) / 1024
        total += kb
        print(f" OK {kb:.0f}KB")

    print(f"\n源文件合计 {total:.0f}KB（编译后约为其 1/6）。记得重新编译烧录。")


if __name__ == "__main__":
    main()
