#!/usr/bin/env python3
"""从**多段式**素材生成五段提示音，写出 firmware/main/tone_pcm.h。

## 为什么要有这个脚本（和 gen_tones_from_wav.py 的区别）

`gen_tones_from_wav.py` 假设素材是**单音**（一个音高 + 一条衰减尾巴），
靠"移调 + 重复"造出五段语义：完成=叮→咚下行，提醒=同音两遍，等等。

但有些素材天生就是**多段式**的 —— 比如本脚本的默认素材
`audio/candidates/feedback-confirm-41.wav`（马林巴）：

    A 短音   0~140ms    350Hz   快速衰减
    B 主音   145~450ms  590Hz   二次起音
    C 收尾   455~712ms  440Hz   三次起音，最饱满

对这种素材，强行"移调造语义"有两个问题：
  1. **方向反了**：素材自带"低→高"走向，而 DONE 要的是"高→低"下行
     （下行在听觉上天然是"结束"）。硬移调只能改音高，改不了走向。
  2. **浪费素材**：三段各具特色，拆成单音等于扔掉一半表现力。

所以本脚本**按素材原结构**设计五段：直接用素材自己的音块组合，
每段取不同的片段/组合，语义靠"取哪几块 + 重复几次"来区分。

## 五段设计

| 段 | 构成 | 语义 |
|---|---|---|
| DONE | A + B + C（完整三段） | 完成、落地：一声完整的马林巴琶音 |
| ATTENTION | B + 间隙 + B | 还没完，等你：同一音重复两遍 |
| CONNECTED | B（单块，短） | 连上了：单声 |
| SESSION_END | A + C | 收工：低起低落，比 DONE 沉 |
| KEY | A（极短切片） | 你按到了：最短最轻 |

## 响度对齐

沿用两步法（与 gen_tones_from_wav.py 一致）：
  1) 五段先按发声区 RMS 拉到同一水平 —— 决定**相对**响度
  2) 再乘**同一个**全局增益，把最响段峰值顶到 92% —— 决定**绝对**响度

⚠ 不能各段各自归一到满幅：马林巴起音尖、衰减长，波峰因数高，
各自归一会让响度被带跑。

⚠ 按键音在**全局增益之后**单独压 -6dB。顺序不能反：反过来的话
peak_max 被这段轻音拉低，全局增益整体抬上去，其余四段反而更响。

## 用法

    python tools/gen_tones_multistruct.py audio/candidates/feedback-confirm-41.wav
    python tools/gen_tones_multistruct.py <素材.wav> --dry-run
"""

from __future__ import annotations

import argparse
import math
import sys
import wave
from pathlib import Path

import numpy as np

RATE = 24000                     # 与 audio.cpp 的 AUDIO_RATE / I2S 一致

# 定位项目根再拼固件路径。别用 parent.parent：本脚本住在 tools/tones/，
# 逐级数层数会在每次整理目录时失效（见 tools/_root.py 的说明）。
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _root import project_root       # noqa: E402

HEADER = project_root() / "firmware" / "main" / "tone_pcm.h"

# 素材的三段边界（ms）。对 feedback-confirm-41 实测得到：
#   A 短音 0~140ms(350Hz) / B 主音 145~450ms(590Hz) / C 收尾 455~712ms(440Hz)
# 换素材时用 --segments "0,140,145,450,455,712" 覆盖。
DEFAULT_SEGMENTS = "0,140,145,450,455,712"

SEG_A = (0, 140)      # 短音 350Hz
SEG_B = (145, 450)    # 主音 590Hz
SEG_C = (455, 712)    # 收尾 440Hz

GAP_ATTN = 90         # 提醒两音之间的间隔
FADE_IN = 3
FADE_OUT = 18
FADE_OUT_KEY = 10     # 110ms 的音用 18ms 淡出会吃掉 1/6，听起来"没响就没了"

DUR_KEY = 110         # 按键音必须短到不打断思路
DUR_CONN = 260        # 连上：主音截到 260ms 足够辨识

# 按键音单独压低的分贝数（在五段响度拉齐**之后**再乘）
KEY_GAIN_DB = -6.0


# ---------------------------------------------------------------- 基础工具

def load_mono(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path)) as w:
        sr, ch, n, sw = w.getframerate(), w.getnchannels(), w.getnframes(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit(f"只支持 16-bit PCM，这个是 {sw*8}-bit")
    a = np.frombuffer(raw, dtype=np.int16).astype(float)
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    return a, sr


def lowpass(x: np.ndarray, fc: float, sr: int, taps: int = 63) -> np.ndarray:
    if fc >= sr / 2 * 0.99:
        return x
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * fc / sr * n) * np.hamming(taps)
    h /= h.sum()
    return np.convolve(x, h, mode="same")


def resample(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """线性插值重采样。降采样前先低通到目标 Nyquist 的 0.9，防折返。"""
    if sr_in == sr_out:
        return x
    if sr_out < sr_in:
        x = lowpass(x, sr_out * 0.45, sr_in)
    n_out = int(len(x) * sr_out / sr_in)
    pos = np.linspace(0, len(x) - 1, n_out)
    i = np.floor(pos).astype(int)
    i = np.clip(i, 0, len(x) - 2)
    frac = pos - i
    return x[i] * (1 - frac) + x[i + 1] * frac


def fade(x: np.ndarray, in_ms: float = 0.0, out_ms: float = 0.0) -> np.ndarray:
    y = x.copy()
    n = len(y)
    k = min(int(in_ms / 1000 * RATE), n)
    for i in range(k):
        y[i] *= 0.5 * (1 - math.cos(math.pi * i / k))
    k = min(int(out_ms / 1000 * RATE), n)
    for i in range(k):
        y[n - k + i] *= 0.5 * (1 + math.cos(math.pi * i / k))
    return y


def trim(x: np.ndarray, ms: float) -> np.ndarray:
    k = int(ms / 1000 * RATE)
    return x[:k] if len(x) > k else x


def voiced_rms(x: np.ndarray) -> float:
    """发声区 RMS：只统计 |x| > 峰值 2% 的样本，避免静音段稀释均值。"""
    pk = float(np.abs(x).max()) or 1.0
    v = x[np.abs(x) > pk * 0.02]
    return float(np.sqrt((v ** 2).mean())) if len(v) else 0.0


def norm(x: np.ndarray, rms_target: float) -> np.ndarray:
    r = voiced_rms(x) or 1.0
    y = x * (rms_target / r)
    pk = float(np.abs(y).max())
    ceiling = 32767 * 0.94
    if pk > ceiling:
        y *= ceiling / pk
    return y


def silence(ms: float) -> np.ndarray:
    return np.zeros(int(ms / 1000 * RATE))


def concat(*parts: np.ndarray) -> np.ndarray:
    return np.concatenate(parts)


def centroid(y: np.ndarray) -> float:
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
    f = np.fft.rfftfreq(len(y), 1 / RATE)
    return float((X * f).sum() / (X.sum() or 1))


# ---------------------------------------------------------------- 主流程

def main() -> int:
    ap = argparse.ArgumentParser(description="从多段式素材生成五段提示音")
    ap.add_argument("source", help="素材 WAV 路径")
    ap.add_argument("--out", default=str(HEADER))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--name", default="", help="写进注释的来源说明")
    ap.add_argument("--segments", default=DEFAULT_SEGMENTS,
                    help="三段边界 ms，格式 a1,a2,b1,b2,c1,c2")
    args = ap.parse_args()

    src = Path(args.source).expanduser()
    if not src.is_file():
        print(f"[ERROR] 找不到 {src}")
        return 1

    vals = [int(v) for v in args.segments.split(",")]
    if len(vals) != 6:
        print(f"[ERROR] --segments 需要 6 个整数，收到 {len(vals)}")
        return 1
    seg_a, seg_b, seg_c = (vals[0], vals[1]), (vals[2], vals[3]), (vals[4], vals[5])

    a, sr = load_mono(src)
    pk = float(np.abs(a).max())
    idx = np.where(np.abs(a) > pk * 0.02)[0]
    if not len(idx):
        print("[ERROR] 素材里没有有效声音")
        return 1
    s = max(0, idx[0] - int(0.005 * sr))
    e = min(len(a), idx[-1] + int(0.05 * sr))
    base = resample(a[s:e], sr, RATE)
    base_ms = len(base) / RATE * 1000
    print(f"素材: {src.name}  {sr}Hz  有效 {(idx[-1]-idx[0])/sr*1000:.0f}ms")
    print(f"      -> 重采样到 {RATE}Hz，{base_ms:.0f}ms")
    print(f"      三段边界 A={seg_a}ms  B={seg_b}ms  C={seg_c}ms\n")

    def cut(seg, pad_tail=0):
        """按 ms 边界切出一段（素材已重采样到 RATE，时间轴与 ms 一一对应）。"""
        a0 = int(seg[0] / 1000 * RATE)
        a1 = int(seg[1] / 1000 * RATE)
        piece = base[a0:a1]
        if len(piece) == 0:
            raise SystemExit(f"段 {seg} 切出来是空的")
        if pad_tail:
            piece = concat(piece, silence(pad_tail))
        return piece

    # ---- 五段 ----
    # DONE：完整三段。这是素材本来的样子，不做任何移调 —— 素材的
    # "低→高→落"走向本身就是"一件事完成了"的自然语感。
    done = concat(
        fade(cut(seg_a), FADE_IN, 0),
        fade(cut(seg_b), FADE_IN, 0),
        fade(cut(seg_c), FADE_IN, FADE_OUT),
    )

    # ATTENTION：主音重复两遍。同音重复制造"未完成感"——
    # 下行在听觉上是"结束"，那是 DONE 的语义，提醒不能用。
    attn_note = fade(cut(seg_b), FADE_IN, 14)
    attn = concat(attn_note, silence(GAP_ATTN), attn_note)

    # CONNECTED：单块主音，短。
    conn = fade(trim(cut(seg_b), DUR_CONN), FADE_IN, 14)

    # SESSION_END：A + C。低起低落，比 DONE 更沉、更"收"。
    end = concat(
        fade(cut(seg_a), FADE_IN, 0),
        fade(cut(seg_c), FADE_IN, FADE_OUT),
    )

    # KEY：短音块的最前一小段，极短且最轻。它是全工程唯一"用户自己
    # 制造的、每次都会听到"的声音，连按时绝不能变成噪音。
    key = fade(trim(cut(seg_a), DUR_KEY), FADE_IN, FADE_OUT_KEY)

    # ---- 响度对齐（两步）----
    ref = voiced_rms(done)
    raw = [
        ("DONE",        "k_tone_done", "TONE_DONE_LEN", norm(done, ref)),
        ("ATTENTION",   "k_tone_attn", "TONE_ATTN_LEN", norm(attn, ref)),
        ("CONNECTED",   "k_tone_conn", "TONE_CONN_LEN", norm(conn, ref)),
        ("SESSION_END", "k_tone_end",  "TONE_END_LEN",  norm(end,  ref)),
        ("KEY",         "k_tone_key",  "TONE_KEY_LEN",  norm(key,  ref)),
    ]
    peak_max = max(float(np.abs(t[3]).max()) for t in raw) or 1.0
    gain = 32767 * 0.92 / peak_max
    tones = [(n, s, m, t * gain) for n, s, m, t in raw]

    # 按键音在**全局增益之后**单独压低。顺序不能反：反过来的话
    # peak_max 会被这段轻音拉低，全局增益整体抬上去，其余四段反而更响。
    key_gain = 10.0 ** (KEY_GAIN_DB / 20.0)
    tones = [(n, s, m, (y * key_gain) if n == "KEY" else y) for n, s, m, y in tones]
    print(f"全局增益 {gain:.2f}x（最响段峰值 {peak_max:.0f} → {peak_max*gain:.0f}）")
    print(f"按键音额外 {KEY_GAIN_DB:+.1f}dB = {key_gain:.3f}x\n")

    print(f"{'段':12} {'时长':>8} {'RMS':>8} {'峰值':>8} {'重心':>8}")
    for name, _, _, y in tones:
        print(f"{name:12} {len(y)/RATE*1000:7.0f}ms {voiced_rms(y):8.0f} "
              f"{np.abs(y).max():8.0f} {centroid(y):7.0f}Hz")
        assert abs(y[0]) < 200 and abs(y[-1]) < 200, f"{name} 首尾没归零"
        assert len(y) < 1200 / 1000 * RATE, f"{name} 超出 AUDIO_MS_MAX"

    if args.dry_run:
        print("\n[dry-run] 未写文件")
        return 0

    # ---- 写头文件 ----
    lines = [
        "// 自动生成，勿手改。",
        "// 重新生成：python tools/gen_tones_multistruct.py <素材.wav>",
        "//",
        f"// 素材：{args.name or src.name}",
        "//",
        "// 素材是**三段式**的（马林巴），五段按素材原结构组合，不做移调：",
        "//",
        f"//   素材段 A  {seg_a[0]:>4d}~{seg_a[1]:<4d}ms  短音 350Hz",
        f"//   素材段 B  {seg_b[0]:>4d}~{seg_b[1]:<4d}ms  主音 590Hz",
        f"//   素材段 C  {seg_c[0]:>4d}~{seg_c[1]:<4d}ms  收尾 440Hz",
        "//",
        "//   段            构成                    时长",
        f"//   DONE          A+B+C（完整三段）      {len(tones[0][3])/RATE*1000:.0f}ms",
        f"//   ATTENTION     B + 间隙 + B          {len(tones[1][3])/RATE*1000:.0f}ms",
        f"//   CONNECTED     B（单块，短）          {len(tones[2][3])/RATE*1000:.0f}ms",
        f"//   SESSION_END   A + C                  {len(tones[3][3])/RATE*1000:.0f}ms",
        f"//   KEY           A（极短切片）          {len(tones[4][3])/RATE*1000:.0f}ms",
        "//",
        "// 为什么保留素材原结构、而不是移调造语义：",
        "//   素材自带「低→高→落」走向，DONE 要的却是「高→低」下行 ——",
        "//   下行在听觉上天然是「结束」，那是 DONE 的语义，而上行是「开始」。",
        "//   移调只能改音高、改不了走向，强行套用会让「完成」听着像「开始」。",
        "//   所以这里按「取哪几块 + 重复几次」区分语义，音色零损失。",
        "//",
        f"// 采样率 {RATE}Hz 与 I2S 输出一致，运行时零重采样。",
        "// 单声道 int16，播放时复制到左右声道（I2S 配的是立体声，",
        "// 直接送单声道会让 L/R 错位，听起来是失真加噪声）。",
        "//",
        "// 响度：五段先按发声区 RMS 拉齐（相对响度），再乘同一个",
        "// 全局增益把最响段顶到 92%（绝对响度）。按键音再单独 -6dB ——",
        "// 它是唯一会被用户连按的声音，做响会迅速变成噪音。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define TONE_RATE    {RATE}",
        "",
    ]

    for name, sym, macro, y in tones:
        lines.append(f"#define {macro} {len(y)}")
    lines.append("")
    for name, sym, macro, y in tones:
        lines.append(f"// {name}  {len(y)/RATE*1000:.0f}ms")
        lines.append(f"static const int16_t {sym}[{macro}] = {{")
        vals = y.astype(np.int16)
        for i in range(0, len(vals), 12):
            row = ", ".join(f"{int(v):>6d}" for v in vals[i:i+12])
            lines.append(f"    {row},")
        lines.append("};")
        lines.append("")

    out = Path(args.out)
    out.write_text("\n".join(lines), encoding="utf-8")
    total_ms = sum(len(t[3]) for t in tones) / RATE * 1000
    print(f"\n已写出 {out}（{out.stat().st_size/1024:.1f}KB，五段合计 {total_ms:.0f}ms）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
