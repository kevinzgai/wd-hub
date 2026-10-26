#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成固件字库用的字符集。

为什么要两套：
  - 28px 只显示时钟（数字）、"WiFi 未连接"、"配网模式"，却因为共用同一份
    字符集吃掉了 214KB —— 三个字库里最贵的那个。给它一份小集就够。
  - 12px / 16px 要显示**服务端下发的会话标题、项目名**，字符不可预知，
    必须留足余量，否则屏幕上就会出现一堆方格。

字符来源（按优先级合并）：
  1. GB2312 一级常用汉字 3755 个 —— 覆盖日常用字 99.7%，是"不会出方格"的底座
  2. 固件源码里所有字符串字面量 —— UI 文案自己必须全含，这条不能漏
  3. 服务端真实数据（/status、/screen/*、数据库里全部会话标题 / 任务名 / 自动化名）
     —— 比闭眼猜准得多
  4. ASCII 全量 + 界面用到的几何符号

用法：
  python tools/gen_charset.py                # 扫本机数据
  python tools/gen_charset.py --no-db        # 不读 WorkBuddy 数据库
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sqlite3
import sys
import urllib.request

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
ROOT      = TOOLS_DIR.parent
MAIN_DIR  = ROOT / "firmware" / "main"

OUT_UI   = TOOLS_DIR / "charset_ui.txt"    # 28px 用：只含 UI 文案
OUT_FULL = TOOLS_DIR / "charset.txt"       # 12/16px 用：含常用汉字底座

# 界面里当图标用的几何符号。
#
# 从 gen_fonts 导入而不是各写一份：这两处必须完全一致，否则会出现
# "字符集里有、但生成时被排除"，白白让自检报警告。之前就是两处各写一份，
# 改了一边忘了另一边。
from gen_fonts import EXTRA_SYMBOLS

# UI 里当分隔符 / 装饰的标点，源码字符串之外单独补
#
# 全角标点必须在这里显式列全：它们落在 GB2312 的 A1 标点区，**不在**一级
# 汉字区，所以常用字底座盖不住。实测任务名里出现 "、：" 时屏上就是两个方格。
EXTRA_PUNCT = (
    "·—…、。，；：？！（）【】《》〈〉「」『』“”‘’～＋－×÷＝＜＞％＆＃＠"
)


def gb2312_level1() -> str:
    """GB2312 一级汉字区（B0A1-D7FE），3755 字，按拼音排序。

    这就是"常用字"最省事的来源，不用额外下字表。
    """
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)


def chars_in_source() -> tuple[set[str], set[str]]:
    """从固件源码里收字符。

    返回 (全部, 仅 UI)。

    **不用正则去提取字符串字面量** —— 试过，会错位。
    `re.findall(r'"((?:[^"\\\\]|\\\\.)*)"', text)` 在源码里一旦遇到引号配对
    被打乱（转义、字符字面量等），就会把中间的整段字符串跳过去。
    实测 `{ "⚙ 工作", "✓ 完成", "⚡ 自动" }` 里的 ⚙ 就是这样漏掉的，
    而它恰好是"完成"前面那个图标 —— 漏掉就是屏上一个方格。

    改成：剥掉注释后，直接收集文件里**所有**非 ASCII 字符。
    代码里除注释外，非 ASCII 只可能出现在字符串字面量里，所以这个口径
    既不会漏、也不会多收。
    """
    allc: set[str] = set()
    uic:  set[str] = set()

    files = sorted(MAIN_DIR.glob("*.cpp")) + sorted(MAIN_DIR.glob("*.h"))
    for f in files:
        if f.name.startswith("font_") or f.name in ("logo_wb.c", "pet_frames.c", "tone_pcm.h"):
            continue
        txt = f.read_text(encoding="utf-8", errors="ignore")

        # 先剥注释。注释里的中文不上屏，收进来纯属浪费。
        clean = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
        clean = re.sub(r"//[^\n]*", "", clean)

        allc.update(ch for ch in clean if ord(ch) > 0x7F)

        # UI 子集：跳过日志行（那些只进串口，不占屏幕字库）
        for line in clean.splitlines():
            if "ESP_LOG" in line or "printf" in line:
                continue
            uic.update(ch for ch in line if ord(ch) > 0x7F)

    return allc, uic


BRIDGE_ENDPOINTS = (
    "/status",
    "/api/v1/board",
    "/api/v1/sessions",
    "/api/v1/tasks",
    "/api/v1/automations",
    "/api/v1/artifacts",
    "/screen/1",
    "/screen/2",
    "/screen/3",
    "/screen/4",
    "/api/ticker",
)


def chars_from_bridge(ports=(27777, 8080)) -> set[str]:
    """从正在跑的服务拉真实下发内容。服务没起也不影响生成。

    为什么要扫**所有**端点：任务名里的全角标点、列表屏的行标题都只在
    特定接口出现，只扫 /status 会漏（实测漏了 "、："）。
    """
    got: set[str] = set()
    for port in ports:
        hit = 0
        for ep in BRIDGE_ENDPOINTS:
            try:
                raw = urllib.request.urlopen(
                    f"http://127.0.0.1:{port}{ep}", timeout=3).read().decode("utf-8")
            except Exception:
                continue
            hit += 1
            try:
                walk_json(json.loads(raw), got)
            except json.JSONDecodeError:
                got.update(raw)      # /api/ticker 是 text/plain
        if hit:
            print(f"  :{port} 收录 {hit}/{len(BRIDGE_ENDPOINTS)} 个端点", file=sys.stderr)
    return {c for c in got if ord(c) > 0x7F}


def walk_json(o, out: set[str]) -> None:
    if isinstance(o, str):
        out.update(o)
    elif isinstance(o, dict):
        for v in o.values():
            walk_json(v, out)
    elif isinstance(o, list):
        for v in o:
            walk_json(v, out)


def chars_from_db() -> set[str]:
    """数据库里的历史标题 —— 将来一定会显示在屏上，现在先备好字。"""
    db = pathlib.Path(os.path.expanduser("~")) / ".workbuddy" / "workbuddy.db"
    if not db.exists():
        print("  未找到 workbuddy.db，跳过", file=sys.stderr)
        return set()

    got: set[str] = set()
    try:
        c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
        tables = {r[0] for r in c.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        # 只取真正会上屏的文本列
        cols = {
            "sessions": ("title", "custom_title", "cwd"),
            "automations": ("name",),
            "tasks": ("name", "title", "subject"),
        }
        for t, cs in cols.items():
            if t not in tables:
                continue
            have = {r[1] for r in c.execute(f"PRAGMA table_info({t})")}
            for col in cs:
                if col not in have:
                    continue
                try:
                    for (v,) in c.execute(f"SELECT {col} FROM {t} WHERE {col} IS NOT NULL"):
                        if isinstance(v, str):
                            got.update(v)
                except sqlite3.Error:
                    pass
        c.close()
        print(f"  数据库收录完成", file=sys.stderr)
    except Exception as e:
        print(f"  读库失败: {e}", file=sys.stderr)
    return {ch for ch in got if ord(ch) > 0x7F}


def norm(chars: set[str]) -> str:
    """去掉不该进字库的字符，排序输出。

    两条硬规则，都是踩过坑的：

    1. **丢掉 BMP 之外的字符**（U+10000 以上）。
       LVGL 的 cmap 表里 unicode_list 是 uint16 的"相对 range_start 偏移"，
       字符码点跨度一旦超过 65535 就没法表达。实测配网页 HTML 里那个 🔒
       (U+1F512) 被扫进来，把跨度撑到 128091，生成出来的 cmap 是坏的。
       反正 emoji 只给浏览器看，LVGL 根本不渲染，直接丢。

    2. 丢掉控制字符。
    """
    bad = set("\r\n\t\x00")
    keep = {c for c in chars
            if c.isprintable() and c not in bad and ord(c) <= 0xFFFF}
    return "".join(sorted(keep))


def main() -> int:
    ap = argparse.ArgumentParser(description="生成固件字库字符集")
    ap.add_argument("--no-db", action="store_true", help="不读 WorkBuddy 数据库")
    ap.add_argument("--no-bridge", action="store_true", help="不拉服务端数据")
    ap.add_argument("--cli-args-limit", type=int, default=100_000,
                    help="字符集超过这个长度就报错（防命令行溢出）")
    args = ap.parse_args()

    print("[1/4] 扫固件源码 ...", file=sys.stderr)
    allc, uic = chars_in_source()
    print(f"      源码字符串 {len(allc)} 字，其中 UI 文案 {len(uic)} 字", file=sys.stderr)

    dyn: set[str] = set()
    if not args.no_bridge:
        print("[2/4] 拉服务端真实数据 ...", file=sys.stderr)
        dyn |= chars_from_bridge()
    if not args.no_db:
        print("[3/4] 读数据库历史标题 ...", file=sys.stderr)
        dyn |= chars_from_db()
    print(f"      动态数据 {len(dyn)} 字", file=sys.stderr)

    print("[4/4] 合成字符集 ...", file=sys.stderr)

    # ---- 28px：源码字符串全覆盖，但不带常用字底座 ----
    #
    # 不带底座是因为它只显示时钟、数字和两三句提示，喂 3755 字纯属浪费
    # （它是三个字库里最贵的，原来 214KB）。
    # 但**不能**只挑 UI 文案：日志字符串里那些字一旦哪天被搬到屏幕上，
    # 就会变成方格。全量源码字符串才 170 字，多这几十个只多约 10KB，
    # 换"永远不会因为文案挪位置而出现方格"，很划算。
    ui_set = set(allc) | set(EXTRA_SYMBOLS) | set(EXTRA_PUNCT)
    ui_out = norm(ui_set)
    OUT_UI.write_text(ui_out, encoding="utf-8")
    print(f"      {OUT_UI.name}: {len(ui_out)} 字（28px 用）", file=sys.stderr)

    # ---- 12/16px：常用字底座 + 源码 + 动态数据 ----
    base = gb2312_level1()
    full_set = set(base) | allc | dyn | set(EXTRA_SYMBOLS) | set(EXTRA_PUNCT)
    full_out = norm(full_set)
    OUT_FULL.write_text(full_out, encoding="utf-8")
    print(f"      {OUT_FULL.name}: {len(full_out)} 字（12/16px 用，含 GB2312 一级 {len(base)} 字底座）",
          file=sys.stderr)

    # 动态数据里有、但底座没收进来的字，单独提一下（都是二级字或符号）
    rare = {c for c in dyn if c not in set(base)}
    if rare:
        print(f"      动态数据里的非常用字 {len(rare)} 个（已并入）: {''.join(sorted(rare))[:80]}",
              file=sys.stderr)

    if len(full_out) > args.cli_args_limit:
        print(f"[!] 字符集 {len(full_out)} 字超过 {args.cli_args_limit}，命令行可能溢出",
              file=sys.stderr)
        return 1

    print("完成。接着跑 tools/gen_fonts.py", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
