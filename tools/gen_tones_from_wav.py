#!/usr/bin/env python3
"""从任意 WAV 素材生成四段提示音，写出 firmware/main/tone_pcm.h。

这是**唯一**的提示音生成脚本。

## 历史

早期有两个脚本同时写 tone_pcm.h，算法完全不同：
  - `gen_tones.py`        依赖一段固定的 mp3（硬编码 C:\\Users\\...\\307888.mp3），
                          素材早已找不到，跑不起来
  - `rebuild_done.py`     假设 DONE 音是旧结构（62ms 叮 + 升八度 1412Hz），
                          而当前产物是 610ms/220ms 叮、**没升八度**

两个都留着 = 谁跑谁毁产物，而且静默产出错误提示音（rebuild_done 跑一次
会把 220ms 的叮在第 62ms 硬切，再对不存在的频率做陷波）。
已删除，只保留本脚本。

## 设计约定（沿用原方案，别随意打破）

四段**同源音色**，靠"音高 + 音数 + 节奏"区分语义，而不是换音色 ——
同源的好处是听感统一，用户不需要重新学习。

    段            结构                   语义
    DONE          高音 → 低音（下行）      完成、落地
    ATTENTION     同一音重复两遍            还没完，等你
    CONNECTED     单声，短                  连上了
    SESSION_END   单声，最低                收工

⚠️ 为什么 ATTENTION 不用下行：下行在听觉上天然是"结束"，那是 DONE 的语义。
提醒要用**同音重复**制造"未完成感"，这也是人催人时的本能节奏。

## 音高怎么定

全部以素材原始音高为基准向下移（只降不升）：
  · 向下重采样不会产生混叠，音质损失最小；向上会缺高频，听感发虚。
  · 板载小喇叭频响下限 300~400Hz，四段都保持在 1.4kHz 以上比较稳。

## 用法

    python tools/gen_tones_from_wav.py 素材.wav
    python tools/gen_tones_from_wav.py 素材.wav --dry-run   # 只出报告
"""

from __future__ import annotations

import argparse
import math
import sys
import wave
from pathlib import Path

import numpy as np

RATE = 24000                     # 与 audio.cpp 的 AUDIO_RATE / I2S 一致
HEADER = Path(__file__).resolve().parent.parent / "firmware" / "main" / "tone_pcm.h"

# 各段相对素材原始音高的半音偏移（负数 = 降低）
SEMI_DONE_HI = 0                 # 叮：保持原音高
SEMI_DONE_LO = -7                # 咚：纯五度下行，小喇叭上分得开
SEMI_ATTN = -3                   # 比 DONE 的叮低 3 个半音，避免"以为是完成了"
SEMI_CONN = -5
SEMI_END = -12                   # 收工：低八度，最沉

# 各段时长上限（ms）。都在 audio.cpp 的 AUDIO_MS_MAX=1200 之内
DUR_CONN = 180
DUR_ATTN_NOTE = 250
DUR_DONE_HI = 220
DUR_DONE_LO = 320
DUR_END = 350

GAP_ATTN = 90                    # 两声"叮"的间隔
GAP_DONE = 70                    # 叮/咚 间隔。太近两个音会叠在一起产生互调失真
FADE_IN = 3
FADE_OUT = 18


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


def resample(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """线性插值重采样。1500Hz 以上有混叠风险，所以先做一次简单低通。"""
    if sr_in == sr_out:
        return x
    # 降采样前低通到**目标** Nyquist 的 0.9，否则高频会折返成刺耳杂音。
    # ⚠ 截止频率必须按 sr_out 算，不能按 sr_in。
    # 原写法 lowpass(x, sr_in * 0.45, sr_in)：44.1k → 24k 时实际截止
    # 19.8k，而目标 Nyquist 只有 12k —— 12k 以上的能量全部折返进可听频段。
    if sr_out < sr_in:
        x = lowpass(x, sr_out * 0.45, sr_in)
    n_out = int(len(x) * sr_out / sr_in)
    pos = np.linspace(0, len(x) - 1, n_out)
    i = np.floor(pos).astype(int)
    i = np.clip(i, 0, len(x) - 2)
    frac = pos - i
    return x[i] * (1 - frac) + x[i + 1] * frac


def lowpass(x: np.ndarray, fc: float, sr: int, taps: int = 63) -> np.ndarray:
    """加窗 sinc 低通（FIR），只用于抗混叠，不追求陡峭。"""
    if fc >= sr / 2 * 0.99:
        return x
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * fc / sr * n) * np.hamming(taps)
    h /= h.sum()
    return np.convolve(x, h, mode="same")


def shift(x: np.ndarray, semitones: float) -> np.ndarray:
    """按半音移调：重采样（时间轴伸缩），不动谐波比例。

    只降不升的用法下，重采样本身不会混叠 —— 相当于把波形拉长。
    """
    if semitones == 0:
        return x.copy()
    factor = 2.0 ** (semitones / 12.0)
    n_out = max(2, int(len(x) / factor))
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
    out = np.concatenate(parts)
    return out


# ---------------------------------------------------------------- 主流程

def main() -> int:
    ap = argparse.ArgumentParser(description="从 WAV 生成四段提示音")
    ap.add_argument("source", help="素材 WAV 路径")
    ap.add_argument("--out", default=str(HEADER))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--name", default="", help="写进注释的来源说明")
    args = ap.parse_args()

    src = Path(args.source).expanduser()
    if not src.is_file():
        print(f"[ERROR] 找不到 {src}")
        return 1

    a, sr = load_mono(src)
    pk = float(np.abs(a).max())
    idx = np.where(np.abs(a) > pk * 0.02)[0]
    if not len(idx):
        print("[ERROR] 素材里没有有效声音")
        return 1
    # 前后各留一点余量，别把起音削掉
    s = max(0, idx[0] - int(0.005 * sr))
    e = min(len(a), idx[-1] + int(0.05 * sr))
    base = resample(a[s:e], sr, RATE)

    print(f"素材: {src.name}  {sr}Hz 有效 {(idx[-1]-idx[0])/sr*1000:.0f}ms")
    print(f"      -> 重采样到 {RATE}Hz，{len(base)/RATE*1000:.0f}ms\n")

    # ---- 四段 ----
    done_hi = fade(trim(shift(base, SEMI_DONE_HI), DUR_DONE_HI), FADE_IN, 8)
    done_lo = fade(trim(shift(base, SEMI_DONE_LO), DUR_DONE_LO), FADE_IN, FADE_OUT)
    done = concat(done_hi, silence(GAP_DONE), done_lo)

    attn_note = fade(trim(shift(base, SEMI_ATTN), DUR_ATTN_NOTE), FADE_IN, 14)
    attn = concat(attn_note, silence(GAP_ATTN), attn_note)

    conn = fade(trim(shift(base, SEMI_CONN), DUR_CONN), FADE_IN, 14)

    end = fade(trim(shift(base, SEMI_END), DUR_END), FADE_IN, 25)

    # ---- 响度对齐 ----
    # 两步走：
    #   1) 四段先按发声区 RMS 拉到同一水平 —— 决定**相对**响度；
    #   2) 再对四段乘**同一个**全局增益，把最响那段的峰值顶到 92%
    #      —— 决定**绝对**响度，同时保证谁都不削顶。
    #
    # ⚠ 别对每段各自归一到满幅：那样响度会被各段的波峰因数带跑
    # （铃音起音尖、衰减长，波峰因数 9 以上），四段听感就不齐了。
    ref = voiced_rms(done)
    raw = [
        ("DONE",        "k_tone_done", "TONE_DONE_LEN", norm(done, ref)),
        ("ATTENTION",   "k_tone_attn", "TONE_ATTN_LEN", norm(attn, ref)),
        ("CONNECTED",   "k_tone_conn", "TONE_CONN_LEN", norm(conn, ref)),
        ("SESSION_END", "k_tone_end",  "TONE_END_LEN",  norm(end,  ref)),
    ]
    peak_max = max(float(np.abs(t[3]).max()) for t in raw) or 1.0
    gain = 32767 * 0.92 / peak_max
    tones = [(n, s, m, t * gain) for n, s, m, t in raw]
    print(f"全局增益 {gain:.2f}x（最响段峰值 {peak_max:.0f} → {peak_max*gain:.0f}）\n")

    print(f"{'段':12} {'时长':>8} {'RMS':>8} {'峰值':>8} {'重心':>8}")
    for name, _, _, y in tones:
        X = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        f = np.fft.rfftfreq(len(y), 1 / RATE)
        cent = float((X * f).sum() / (X.sum() or 1))
        print(f"{name:12} {len(y)/RATE*1000:7.0f}ms {voiced_rms(y):8.0f} "
              f"{np.abs(y).max():8.0f} {cent:7.0f}Hz")
        assert abs(y[0]) < 200 and abs(y[-1]) < 200, f"{name} 首尾没归零"
        assert len(y) < 1200 / 1000 * RATE, f"{name} 超出 AUDIO_MS_MAX"

    if args.dry_run:
        print("\n[dry-run] 未写文件")
        return 0

    # ---- 写头文件 ----
    hdr = [
        "// 自动生成，勿手改。重新生成：python tools/gen_tones_from_wav.py <素材.wav>",
        "//",
        f"// 素材：{args.name or src.name}",
        "// 四段同源，靠音高 + 音数 + 节奏区分语义：",
        "//",
        "//   段            结构              相对音高      时长",
        f"//   DONE          高→低 下行         {SEMI_DONE_HI:+d} / {SEMI_DONE_LO:+d}       {len(tones[0][3])/RATE*1000:.0f}ms",
        f"//   ATTENTION     同音重复两遍       {SEMI_ATTN:+d}             {len(tones[1][3])/RATE*1000:.0f}ms",
        f"//   CONNECTED     单声，短           {SEMI_CONN:+d}             {len(tones[2][3])/RATE*1000:.0f}ms",
        f"//   SESSION_END   单声，最低         {SEMI_END:+d}            {len(tones[3][3])/RATE*1000:.0f}ms",
        "//",
        f"// 采样率 {RATE}Hz 与 I2S 输出一致，运行时零重采样。",
        "// 单声道 int16，播放时复制到左右声道（I2S 配的是立体声，",
        "// 直接送单声道会让 L/R 错位，听起来是失真加噪声）。",
        "//",
        f"// 音高只降不升：向下重采样不产生混叠；向上会缺高频，听感发虚。",
        f"// 板载小喇叭频响下限 300~400Hz，四段都保持在 1.4kHz 以上。",
        "//",
        "// 为什么 DONE 的两音之间要留间隔：",
        "//   共源音色下两个音只差几个半音，若首尾相接会全程叠在一起，",
        "//   1.5W 小喇叭直接互调失真（实测用户反馈「沙哑、嗡嗡」）。",
        f"//   留 {GAP_DONE}ms 让前一个衰减下去，两个音才听得开。",
        "//",
        f"// 四段响度已拉齐（按发声区 RMS，静音段不参与平均）。",
        f"// 总体积 {sum(len(t[3]) for t in tones)*2/1024:.1f}KB。",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define TONE_RATE    {RATE}",
        "",
    ]

    body = []
    for _, sym, len_macro, y in tones:
        yi = np.clip(np.round(y), -32768, 32767).astype(int)
        body.append(f"#define {len_macro} {len(yi)}")
        body.append(f"static const int16_t {sym}[{len_macro}] = {{")
        for i in range(0, len(yi), 12):
            body.append("    " + ", ".join("%6d" % v for v in yi[i:i + 12]) + ",")
        body.append("};")
        body.append("")

    Path(args.out).write_text("\n".join(hdr + body), encoding="utf-8")
    print(f"\n[+] 已写入 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
