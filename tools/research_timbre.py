# -*- coding: utf-8 -*-
"""对比实验：找出"电子音感"的真正来源。

当前实现（v2）已经有和弦 + 非谐泛音 + 指数衰减，但用户反馈"还是像电子音"。
本脚本用三个可开关的实验维度，逐项量化它对"自然度"的影响：

  A. 起音噪声（attack transient）——真实敲击的槌击声
  B. 泛音独立衰减（spectral decay）——音色由亮变暗
  C. 微失谐（detune / beating）——声音轻微起伏

不参与构建，纯研究脚本。
"""
import math
import random

RATE = 24000
HARM_MUL2, HARM_MUL3 = 2.76, 5.40
HARM_AMP2, HARM_AMP3 = 0.32, 0.11
LIMIT = 10800.0
SOFT_LIMIT = 29000.0
CHORD_NORM = 1.0 / math.sqrt(3)


def synth(hits, vol=20, use_noise=False, use_spectral_decay=False,
          use_detune=False, seed=1):
    """可开关三个维度的合成器。

    每个振荡器持有自己的频率、幅度、衰减率 —— 这是实现 B 的前提。
    """
    rng = random.Random(seed)
    out, peak = [], 0.0
    attack = RATE * 2 // 1000          # 2ms，打击乐应该更快

    for f0, f1, f2, ms, gain in hits:
        frames = RATE * ms // 1000
        if frames <= 0:
            continue

        # 每个振荡器：[2cos(w), 幅度, 衰减指数倍率, 当前值, 前值, 当前包络]
        osc = []
        for b in (f0, f1, f2):
            if not b:
                continue
            f = float(b)
            # 基频衰减最慢，泛音衰减更快 —— 真实乐器的铁律：
            # 高频能量通过空气和材料散失得更快，所以音色由亮变暗。
            table = [
                (1.0, 1.0, 1.0),
                (HARM_MUL2, HARM_AMP2, 2.6 if use_spectral_decay else 1.0),
                (HARM_MUL3, HARM_AMP3, 4.5 if use_spectral_decay else 1.0),
            ]
            for mul, amp, dmul in table:
                ff = f * mul
                if mul > 1.0 and ff >= LIMIT:
                    continue
                # 微失谐：倍频程越高偏得越多（材料刚度不均匀在高频放大）
                if use_detune:
                    ff *= 1.0 + rng.uniform(-1, 1) * 0.0016 * mul
                w = 2 * math.pi * ff / RATE
                osc.append({
                    "c": 2 * math.cos(w),
                    "prev": -math.sin(w),
                    "cur": 0.0,
                    "amp": CHORD_NORM * amp,
                    "es": math.exp(-4.0 * dmul / frames),
                    "env": 1.0,
                })

        amp_base = (vol / 100.0) * 30000.0 * (gain / 100.0)
        noise_len = RATE * 12 // 1000
        noise_amp = 0.18 * amp_base

        for i in range(frames):
            v = 0.0
            for o in osc:
                v += o["cur"] * o["amp"] * o["env"]
                nx = o["c"] * o["cur"] - o["prev"]
                o["prev"], o["cur"] = o["cur"], nx
                o["env"] *= o["es"]

            # 2ms 陡起音（打击乐的敲击感来自这里，不能省）
            ramp = i / attack if i < attack else 1.0
            samp = v * ramp * amp_base

            if use_noise and i < noise_len:
                samp += rng.uniform(-1, 1) * noise_amp * math.exp(-5.0 * i / noise_len)

            samp = SOFT_LIMIT * math.tanh(samp / SOFT_LIMIT)
            peak = max(peak, abs(samp))
            out.append(samp)
    return out, peak


def zero_cross_rate(x):
    """零交叉率 → 近似谱重心。高频占比越高，交叉越频繁。"""
    if len(x) < 2:
        return 0.0
    zc = sum(1 for i in range(1, len(x)) if (x[i - 1] < 0) != (x[i] < 0))
    return zc / (len(x) - 1)


def centroid_trend(x):
    """后半段/前半段的谱重心比。<0.8 表示音色明显变暗（真实乐器特征）。"""
    h = len(x) // 2
    a = zero_cross_rate(x[:h])
    b = zero_cross_rate(x[h:])
    return b / a if a > 0 else 0.0


def envelope_roughness(x, win=240):
    """包络相对起伏。拍频会让它变大。"""
    env = []
    for i in range(0, len(x) - win, win):
        seg = x[i:i + win]
        env.append(math.sqrt(sum(v * v for v in seg) / win))
    if len(env) < 3:
        return 0.0
    m = sum(env) / len(env)
    if m == 0:
        return 0.0
    var = sum((e - m) ** 2 for e in env) / len(env)
    return math.sqrt(var) / m


DONE_HIT = [(1047, 1319, 1568, 280, 200)]


def main():
    print("=== 单开各维度，看指标变化 ===")
    print("  (谱重心趋势 <0.8 = 音色变暗；包络粗糙度 >0.1 = 有可闻起伏)\n")
    print("  {:26s} {:>12s} {:>12s} {:>8s}".format("配置", "谱重心趋势", "包络粗糙度", "峰值"))
    cfgs = [
        ("v2 现状（三项全关）", {}),
        ("A 起音噪声", {"use_noise": True}),
        ("B 泛音独立衰减", {"use_spectral_decay": True}),
        ("C 微失谐", {"use_detune": True}),
        ("A+B+C 全开", {"use_noise": True, "use_spectral_decay": True,
                        "use_detune": True}),
    ]
    for name, kw in cfgs:
        out, peak = synth(DONE_HIT, **kw)
        print("  {:26s} {:12.2f} {:12.3f} {:8.0f}"
              .format(name, centroid_trend(out), envelope_roughness(out), peak))


if __name__ == "__main__":
    main()
