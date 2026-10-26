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
import re
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT      = os.path.dirname(TOOLS_DIR)
MAIN_DIR  = os.path.join(ROOT, "firmware", "main")

# 界面里当图标用的几何符号。
#
# 注意：这里列的是"可能用到"的候选集，gen_fonts.py 会自动探测两个字体谁有
# 字形、以及谁都没有，缺的会在生成后报出来。
# ⏰⏱⏸（闹钟/秒表/暂停）原本在 Windows 的 Segoe UI Symbol 里有，macOS 的
# Arial Unicode 和 Apple Symbols 都没有，界面也没用到，所以去掉 —— 留着只会
# 让每次构建都报一条无意义的"缺字形"。
EXTRA_SYMBOLS = "●○◆★☆▲△▼■□✓✔✗⚡♥♪☀⚙▶☰✈↔→↑↓"

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


# ---------------------------------------------------------------------------
# cmap 解析：判断某个字最终有没有字形
#
# 直接读生成的 C 文件，比"看起来生成了"可靠 —— lv_font_conv 对字体里没有的
# 字符是**静默跳过**的，只有解析 cmap 才知道到底进了没有。
# ---------------------------------------------------------------------------
def _parse_cmap(src: str):
    """返回 [(range_start, range_length, glyph_id_start, unicode_list_name, type)]"""
    out = []
    pat = (r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),"
           r"\s*\.unicode_list = (\w+),\s*\.glyph_id_ofs_list = \w+,"
           r"\s*\.list_length = \d+,\s*\.type = LV_FONT_FMT_TXT_CMAP_(\w+)")
    for m in re.finditer(pat, src, re.S):
        out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                    m.group(4), m.group(5)))
    return out


def _unicode_list(src: str, name: str):
    if name == "NULL":
        return None
    m = re.search(rf"static const uint16_t {name}\[\]\s*=\s*\{{(.*?)\}};", src, re.S)
    if not m:
        return None
    return [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))]


def has_glyph(src: str, cp: int) -> bool:
    for start, length, gid, lst, typ in _parse_cmap(src):
        if not (start <= cp < start + length):
            continue
        if typ == "FORMAT0_TINY":
            return True                      # 连续映射：范围内就有
        vals = _unicode_list(src, lst)
        return bool(vals) and (cp - start) in vals
    return False


def render_and_read(chars: str, font: str, entry: str, size: int = 16) -> str:
    """用指定字体单独生成一份字库，返回 C 源码（用于探测字形覆盖）。"""
    tmp = tempfile.NamedTemporaryFile(suffix=".c", delete=False)
    tmp.close()
    try:
        subprocess.run(
            ["node", entry, "--font", font, "--symbols", chars,
             "--size", str(size), "--bpp", "4", "--format", "lvgl",
             "--no-compress", "--lv-include", "lvgl.h",
             "--lv-font-name", "probe", "-o", tmp.name],
            capture_output=True, text=True, check=True)
        with open(tmp.name, encoding="utf-8", errors="ignore") as f:
            return f.read()
    finally:
        os.unlink(tmp.name)


def split_symbols(cjk: str, sym: str, entry: str):
    """把符号拆成两组：主字体自己能画的 / 必须靠符号字体的。

    为什么必须拆干净：同一个字符在两个 --font 里都声明，lv_font_conv 会出
    问题 —— 实测 Apple Symbols 的 ✓(U+2713) 单独生成正常，一旦作为第二字体
    合并进来就整颗丢失，屏上显示成方框（"✓ 完成" 前面就是这么变方的）。
    规则：谁先声明就让谁独占，绝不重复。
    """
    src = render_and_read(EXTRA_SYMBOLS, cjk, entry)
    own, need = [], []
    for ch in EXTRA_SYMBOLS:
        (own if has_glyph(src, ord(ch)) else need).append(ch)

    # 主字体画不了的，确认符号字体真的能画，否则报出来
    src2 = render_and_read("".join(need), sym, entry) if need else ""
    lost = [ch for ch in need if not has_glyph(src2, ord(ch))]
    if lost:
        print(f"[!] 两个字体都没有这些符号的字形，屏上会是方框: {''.join(lost)}")
    return "".join(own), "".join(c for c in need if c not in lost)


def glyph_set(src: str) -> set[int]:
    """把字库的 cmap 展开成"有字形的码点集合"，便于批量核对。"""
    cps: set[int] = set()
    cache: dict[str, list[int]] = {}
    for start, length, gid, lst, typ in _parse_cmap(src):
        if typ == "FORMAT0_TINY":
            cps.update(range(start, start + length))
        else:
            if lst not in cache:
                cache[lst] = _unicode_list(src, lst) or []
            cps.update(start + v for v in cache[lst])
    return cps


def verify(path: str, charset: str, label: str) -> int:
    """生成后自检：字符集里的字，字库里到底有没有字形。

    没做这一步之前，"符号丢了"是无声的 —— 字库文件照样生成成功，
    只有等用户看到屏上的方框才知道。这里把它变成构建时就能看见的警告。
    """
    with open(path, encoding="utf-8", errors="ignore") as f:
        cps = glyph_set(f.read())

    missing = [c for c in charset if ord(c) not in cps]
    if not missing:
        return 0

    # 符号缺失最刺眼（"✓ 完成"变"▯ 完成"），单独点名
    lost_syms = [c for c in missing if c in EXTRA_SYMBOLS]
    print(f"    [!] {label}: {len(missing)} 字没有字形")
    if lost_syms:
        print(f"        符号丢失: {''.join(lost_syms)}")
    others = [c for c in missing if c not in EXTRA_SYMBOLS]
    if others:
        print(f"        其他: {''.join(others[:40])}"
              + (" ..." if len(others) > 40 else ""))
    return len(missing)


def main():
    cjk = pick(CJK_CANDIDATES, "中文")
    sym = pick(SYM_CANDIDATES, "符号")
    print(f"中文字体: {cjk}")
    print(f"符号字体: {sym}")

    entry = os.path.join(TOOLS_DIR, "node_modules", "lv_font_conv", "lv_font_conv.js")
    if not os.path.exists(entry):
        sys.exit(f"找不到 lv_font_conv: {entry}\n先跑: cd tools && npm install "
                 f"--registry=https://registry.npmmirror.com")

    # 符号归属：主字体能画的归主字体，剩下的才交给符号字体。绝不重复声明。
    own_syms, need_syms = split_symbols(cjk, sym, entry)
    print(f"符号分工: 中文字体 {len(own_syms)} 个（{own_syms}），"
          f"符号字体 {len(need_syms)} 个（{need_syms}）")
    for ch in EXTRA_SYMBOLS:
        if ch not in own_syms and ch not in need_syms:
            # 已在上面的 split_symbols 里报警，这里不重复
            pass

    total = 0
    bad = 0
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
            # 只声明"主字体画不了"的那部分，避免同字符重复声明导致丢失
            "--font", sym, "--symbols", need_syms,
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

        bad += verify(out, charset + own_syms + need_syms, f"{name}")

    print(f"\n源文件合计 {total:.0f}KB（编译后约为其 1/6）。")
    if bad:
        print(f"[!] 共 {bad} 字缺字形 —— 屏上会显示成方框，检查上面的清单。")
        return 1
    print("全部字符都有字形，可以编译烧录。")
    return 0


if __name__ == "__main__":
    main()
