#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
重新生成固件用的点阵字库（中文 + 符号 合体）。

背景：
  LVGL 的一个 label 只能用一个字体，想在中文旁边显示 ★ ● ⚡ 这类符号，
  要么做 fallback 链，要么把符号并进同一个字体。这里选后者 —— 用 lv_font_conv
  的多 --font 能力，一次生成同时含中文和符号的字体，label 里直接写就行。

为什么不用彩色 emoji：
  Segoe UI Emoji 是 COLR 矢量彩色字体，lv_font_conv 1.5.3 不支持彩色字形，
  栅格化出来只有实心剪影（实测是黑块），没法用。Segoe UI Symbol 是单色字体，
  渲染干净，是 240x240 小屏的正确选择。

用法：
  python tools/gen_fonts.py
"""

import io
import os
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT      = os.path.dirname(TOOLS_DIR)
MAIN_DIR  = os.path.join(ROOT, "firmware", "main")

FONT_CJK = "C:/Windows/Fonts/simhei.ttf"        # 中文：黑体
FONT_SYM = "C:/Windows/Fonts/seguisym.ttf"      # 符号：Segoe UI Symbol（单色）

CHARSET_FILE = os.path.join(TOOLS_DIR, "charset.txt")

# 额外补进去的符号（几何图形 / 状态 / 图标感强的）
EXTRA_SYMBOLS = "●○◆★☆▲△▼■□✓✔✗⚡♥♪☀⏱⏰⚙▶⏸☰✈↔→↑↓"

# 字号 -> 变量名
SIZES = [(12, "font_cn_12"), (16, "font_cn_16"), (28, "font_cn_28")]


def main():
    for fp in (FONT_CJK, FONT_SYM, CHARSET_FILE):
        if not os.path.exists(fp):
            sys.exit(f"找不到: {fp}")

    charset = io.open(CHARSET_FILE, encoding="utf-8").read().strip()
    # 界面里用作分隔符的标点，charset.txt 里没有，单独补
    charset += "·—…"

    # Windows 下 node_modules/.bin/lv_font_conv 是 sh 脚本，不能直接 exec，
    # 用 node 跑它的 JS 入口
    entry = os.path.join(TOOLS_DIR, "node_modules", "lv_font_conv", "lv_font_conv.js")
    if not os.path.exists(entry):
        sys.exit(f"找不到 lv_font_conv: {entry}（先 npm install）")

    for size, name in SIZES:
        out = os.path.join(MAIN_DIR, f"{name}.c")
        cmd = [
            "node", entry,
            "--font", FONT_CJK, "--symbols", charset,
            # 关键：--symbols 走的是字符串参数，空格会被命令行吞掉，
            # 结果字库 ASCII 从 33(!) 开始，U+0020 缺失。界面里
            # "● 连接中" 这种带空格的串，空格就会渲染成占位方框。
            # 用 --range 显式把 0x20~0x7E 全包进来。
            "--range", "0x20-0x7E",
            "--font", FONT_SYM, "--symbols", EXTRA_SYMBOLS,
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            # 关键：必须关掉位图压缩。压缩后的字体 bitmap_format=1，
            # 需要 LV_USE_FONT_COMPRESSED 打开才能解压，否则 LVGL 把所有字
            # 都画成占位方框（中文、数字、符号一起变方格）。
            "--no-compress",
            "--lv-include", "lvgl.h",
            "--lv-font-name", name,
            "-o", out,
        ]
        print(f"[{size}px] 生成 {name}.c ...", end="", flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(" 失败")
            print(r.stdout[-2000:])
            print(r.stderr[-2000:])
            sys.exit(1)
        kb = os.path.getsize(out) / 1024
        print(f" OK {kb:.0f}KB")

    print("完成。记得重新编译烧录。")


if __name__ == "__main__":
    main()
