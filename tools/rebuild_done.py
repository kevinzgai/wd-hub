#!/usr/bin/env python3
"""修 DONE 提示音的"沙哑嗡嗡"——把重叠的叮/咚拆开。

## 问题（2026-10-26 实测）

用户反馈 DONE（完成音）沙哑、嗡嗡。逐样本分析后定位到根因：
**「叮」和「咚」在时间上重叠，而且频率只差 5 个半音。**

    DONE 是整段原始素材升 +12 半音：
        叮  706Hz → 1412Hz
        咚  525Hz → 1040Hz      ← 两者只差 372Hz

比对四段的频谱占比（1380-1460Hz / 1000-1100Hz）：

    时段        DONE（叮+咚）            END（纯咚）
    96ms        1040Hz 79.2% + 1400Hz 15.3%   两者都是 0.0%
    120ms       1040Hz 22.8% + 1400Hz 57.9%   两者都是 0.0%
    144ms       1040Hz 29.1% + 1400Hz 56.9%   两者都是 0.0%

**「咚」本身完全没有 1040/1400Hz 成分** —— DONE 尾段那些 1400Hz 全是
「叮」没衰减完的残留。两个富谐波音在 1.5W 小喇叭上同时存在 → 互调失真
→ 听感就是"沙哑、嗡嗡震动"。

横向对照也支持这个结论：四段里只有 DONE 是两个音**无间隔**叠加；
ATTENTION 也是两个"叮"，但中间留 90ms，用户没反馈它有问题。

## 修法

在不重新拿原始 mp3 的前提下，对现有 PCM 做三件事：

  1. 在 62ms（叮已经衰减到 RMS≈2850、咚刚要起来的位置）切成两段；
  2. 对"咚"段做 1412Hz 陷波（Q=5，-30dB），抽掉"叮"的残留尾巴。
     已验证：残留从最高 39.2% 降到 0.1~1.0%，而"咚"的 1040Hz
     相对占比从 23% 升到 92~99% —— 咚本身不受损；
  3. 中间插 70ms 静音，两端各做 6ms/3ms 淡入淡出。

响度按处理前的 RMS 归一，保持四段之间响度一致（不靠改峰值拉响度）。

## 用法

    python tools/rebuild_done.py            # 原地改写 tone_pcm.h
    python tools/rebuild_done.py --dry-run  # 只打印指标，不写文件
    python tools/rebuild_done.py --gap 90   # 换间隔（默认 70ms）
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path

import numpy as np

RATE = 24000
HEADER = Path(__file__).resolve().parent.parent / "firmware" / "main" / "tone_pcm.h"

CUT_MS = 62.0        # 叮/咚 切点
CUT_FADE_MS = 6.0    # 叮段末尾淡出
IN_FADE_MS = 3.0     # 咚段开头淡入
NOTCH_HZ = 1412.0    # 叮 升八度后的频率，就是残留的位置
NOTCH_Q = 5.0
NOTCH_DB = -30.0
OUT_FADE_MS = 15.0   # 整段末尾淡出，保持和原来一致


def parse_tone(src: str, name: str) -> np.ndarray:
    m = re.search(rf"k_tone_{name}\[TONE_\w+_LEN\]\s*=\s*\{{(.*?)\n\}};", src, re.S)
    if not m:
        raise SystemExit(f"没找到 k_tone_{name}")
    return np.array([int(v) for v in re.findall(r"-?\d+", m.group(1))], dtype=float)


def notch(sig: np.ndarray, f0: float, q: float, depth_db: float) -> np.ndarray:
    """RBJ band-stop biquad。"""
    w0 = 2 * math.pi * f0 / RATE
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2 * q)
    a = 10.0 ** (depth_db / 40.0)          # 中心频率处的增益
    b0, b1, b2 = 1 + alpha * a, -2 * cw, 1 - alpha * a
    a0, a1, a2 = 1 + alpha / a, -2 * cw, 1 - alpha / a
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0

    y = np.zeros(len(sig))
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(sig):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, v
        y2, y1 = y1, o
        y[i] = o
    return y


def fade(x: np.ndarray, in_ms: float = 0.0, out_ms: float = 0.0) -> np.ndarray:
    """余弦淡入淡出，避免端点留折角（折角 = 咔声）。"""
    y = x.copy()
    n = len(y)
    k = min(int(in_ms / 1000 * RATE), n)
    for i in range(k):
        y[i] *= 0.5 * (1 - math.cos(math.pi * i / k))
    k = min(int(out_ms / 1000 * RATE), n)
    for i in range(k):
        y[n - k + i] *= 0.5 * (1 + math.cos(math.pi * i / k))
    return y


def band_pct(seg: np.ndarray, lo: float, hi: float) -> float:
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / RATE)
    tot = X.sum() or 1e-9
    return X[(f >= lo) & (f <= hi)].sum() / tot * 100


def voiced_rms(x: np.ndarray) -> float:
    """发声区 RMS：只统计 |x| > 峰值 2% 的样本。

    ⚠ 不要直接对整段求 RMS。ATTENTION 有 90ms 静音、DONE 现在多了 70ms，
    静音会把平均值稀释掉，导致"同样的听感"算出不同的数字 —— 四段响度
    就散了。按发声样本算，各段才可比。
    """
    if not len(x):
        return 0.0
    pk = float(np.abs(x).max()) or 1.0
    v = x[np.abs(x) > pk * 0.02]
    if not len(v):
        return 0.0
    return float(np.sqrt((v ** 2).mean()))


def main() -> int:
    ap = argparse.ArgumentParser(description="修 DONE 提示音重叠")
    ap.add_argument("--gap", type=float, default=70.0, help="叮/咚 间隔 ms")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--out", default=str(HEADER))
    args = ap.parse_args()

    src = Path(args.out).read_text(encoding="utf-8")
    x = parse_tone(src, "done")
    rms_before = voiced_rms(x)

    ci = int(CUT_MS / 1000 * RATE)
    ding = fade(x[:ci], in_ms=0.0, out_ms=CUT_FADE_MS)
    dong = notch(x[ci:], NOTCH_HZ, NOTCH_Q, NOTCH_DB)
    dong = fade(dong, in_ms=IN_FADE_MS, out_ms=OUT_FADE_MS)

    gap = np.zeros(int(args.gap / 1000 * RATE))
    y = np.concatenate([ding, gap, dong])

    # 响度归一：按发声区 RMS 对齐处理前，四段之间的响度关系保持不变。
    rms_now = voiced_rms(y) or 1.0
    y *= rms_before / rms_now

    # 峰值上限 96%，和 gen_tones.py 的口径一致
    pk = float(np.abs(y).max())
    if pk > 32767 * 0.96:
        y *= 32767 * 0.96 / pk

    yi = np.clip(np.round(y), -32768, 32767).astype(int)

    print(f"原: {len(x):5} 样本 {len(x)/RATE*1000:5.0f}ms  "
          f"RMS={rms_before:6.0f} 峰值={np.abs(x).max():5.0f}")
    print(f"新: {len(yi):5} 样本 {len(yi)/RATE*1000:5.0f}ms  "
          f"RMS={np.sqrt((yi**2).mean()):6.0f} 峰值={np.abs(yi).max():5.0f}")
    print(f"    叮 0-{CUT_MS:.0f}ms | 静音 {args.gap:.0f}ms | "
          f"咚 {CUT_MS+args.gap:.0f}-{len(yi)/RATE*1000:.0f}ms")

    # 验证：咚段不应再有 1412Hz 残留
    dstart = ci + len(gap)
    print("\n  咚段的 1400Hz 残留（应 < 3%）:")
    for off in (0, 34, 58, 82, 106):
        s, e = dstart + int(off / 1000 * RATE), dstart + int(off / 1000 * RATE) + 576
        if e > len(yi):
            break
        seg = yi[s:e].astype(float)
        print(f"    +{off:3}ms  1400Hz={band_pct(seg,1380,1460):5.1f}%  "
              f"1040Hz={band_pct(seg,1000,1100):5.1f}%")

    # 首尾必须归零，否则播放起止会有阶跃咔声
    assert abs(yi[0]) < 200 and abs(yi[-1]) < 200, "首尾没归零"

    if args.dry_run:
        print("\n[dry-run] 未写文件")
        return 0

    # 重新排版数组，每行 12 个，和原文件风格一致
    lines = []
    for i in range(0, len(yi), 12):
        lines.append("    " + ", ".join("%6d" % v for v in yi[i:i + 12]) + ",")
    body = "static const int16_t k_tone_done[TONE_DONE_LEN] = {\n" + \
           "\n".join(lines) + "\n};"

    new_src = re.sub(r"#define TONE_DONE_LEN \d+",
                     f"#define TONE_DONE_LEN {len(yi)}", src, count=1)
    new_src = re.sub(r"static const int16_t k_tone_done\[TONE_DONE_LEN\]\s*=\s*\{.*?\n\};",
                     body, new_src, count=1, flags=re.S)
    Path(args.out).write_text(new_src, encoding="utf-8")
    print(f"\n[+] 已写入 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
