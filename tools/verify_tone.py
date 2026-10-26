# -*- coding: utf-8 -*-
"""提示音合成算法的离线仿真，用来验证音频参数。

不参与构建、不影响固件。用途：
  1) 确认峰值不削顶（三个频率叠加 + 响度补偿很容易超过 int16 上限）；
  2) 确认频谱里确实有基频和非谐泛音（否则"钟琴音色"只是注释里的空话）；
  3) 确认四段音效的**感知响度**基本齐平（峰值齐平不等于听着一样响）。

参数必须和 firmware/main/audio.cpp 保持一致，改那边就顺手改这里。
"""
import math

RATE = 24000
HARM_MUL2, HARM_MUL3 = 2.76, 5.40
HARM_AMP2, HARM_AMP3 = 0.32, 0.11
LIMIT = 10800.0
SOFT_LIMIT = 29000.0
CHORD_NORM = 1.0 / math.sqrt(3)


def synth(hits, vol=20):
    """复现 audio.cpp 的 synth()：递推振荡器 + 非谐泛音 + 指数衰减 + 软限幅。"""
    out, peak = [], 0.0
    attack = RATE * 3 // 1000
    for f0, f1, f2, ms, gain in hits:
        frames = RATE * ms // 1000
        if frames <= 0:
            continue
        fs, amps = [], []
        for b in (f0, f1, f2):
            if not b:
                continue
            f = float(b)
            fs.append(f)
            amps.append(CHORD_NORM)
            if f * HARM_MUL2 < LIMIT:
                fs.append(f * HARM_MUL2)
                amps.append(CHORD_NORM * HARM_AMP2)
            if f * HARM_MUL3 < LIMIT:
                fs.append(f * HARM_MUL3)
                amps.append(CHORD_NORM * HARM_AMP3)

        c = [2 * math.cos(2 * math.pi * f / RATE) for f in fs]
        prev = [-math.sin(2 * math.pi * f / RATE) for f in fs]
        cur = [0.0] * len(fs)
        env, es = 1.0, math.exp(-4.0 / frames)
        amp_base = (vol / 100.0) * 30000.0 * (gain / 100.0)

        for i in range(frames):
            v = 0.0
            for j in range(len(fs)):
                v += cur[j] * amps[j]
                nx = c[j] * cur[j] - prev[j]
                prev[j], cur[j] = cur[j], nx
            e = env * (i / attack if i < attack else 1.0)
            env *= es
            s = v * e * amp_base
            s = SOFT_LIMIT * math.tanh(s / SOFT_LIMIT)
            peak = max(peak, abs(s))
            out.append(s)
    return out, peak


def goertzel(x, f, n=RATE // 20):
    """测单个频率的能量，用来确认泛音真的存在。"""
    k = 2 * math.pi * f / RATE
    cc = 2 * math.cos(k)
    s1 = s2 = 0.0
    for i in range(min(n, len(x))):
        s0 = x[i] + cc * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - cc * s1 * s2))


def loudness(x):
    """简易感知响度：20ms 窗 RMS 的均值，近似耳朵的时间积分。

    这个指标比峰值更接近"听着有多响"。指数衰减的包络峰值很高但
    能量集中在前 100ms，耳朵积分后会觉得比同峰值的持续音轻得多。
    """
    w = RATE // 50
    acc = []
    for i in range(0, len(x) - w, w):
        seg = x[i:i + w]
        acc.append(math.sqrt(sum(v * v for v in seg) / w))
    return sum(acc) / len(acc) if acc else 0.0


SETS = {
    "DONE": [(784, 988, 1175, 140, 160), (0, 0, 0, 25, 0),
             (1047, 1319, 1568, 280, 200)],
    "SESSION": [(880, 1047, 1319, 240, 180)],
    "ATTENTION": [(1319, 1568, 0, 110, 220), (0, 0, 0, 35, 0),
                  (880, 1047, 0, 190, 250)],
    "CONNECTED": [(2093, 0, 0, 55, 340), (0, 0, 0, 15, 0),
                  (3136, 0, 0, 60, 340)],
}


def main():
    print("=== 峰值与响度（int16 上限 32767）===")
    print("  {:10s} {:>7s} {:>8s} {:>8s} {:>10s}".format("段", "时长", "峰值", "感知响度", "状态"))
    louds = []
    for name, hits in SETS.items():
        out, peak = synth(hits)
        ms = len(out) / RATE * 1000
        ld = loudness(out)
        louds.append((name, ld))
        state = "削顶!!" if peak > 32767 else "OK"
        print("  {:10s} {:6.0f}ms {:8.0f} {:8.0f} {:>10s}"
              .format(name, ms, peak, ld, state))

    vals = [v for _, v in louds]
    lo, hi = min(vals), max(vals)
    print("  响度范围 {:.0f}~{:.0f}，极差 {:.1f}%（越小越好，目标 <20%）"
          .format(lo, hi, (hi - lo) / hi * 100))

    print()
    print("=== 频谱检查：DONE 第一个和弦 G5+B5+D6 ===")
    out, _ = synth([(784, 988, 1175, 140, 160)])
    seg = out[200:2000]
    for f in (784, 988, 1175):
        e1 = goertzel(seg, f)
        e2 = goertzel(seg, f * HARM_MUL2)
        e3 = goertzel(seg, f * HARM_MUL3)
        print("  基频 {:7.1f}Hz {:7.1f} | 泛音 {:7.1f}Hz {:6.1f} | {:7.1f}Hz {:6.1f}"
              .format(f, e1, f * HARM_MUL2, e2, f * HARM_MUL3, e3))
    print("  （泛音能量应明显小于基频但非零，且 2.76 倍比 5.40 倍强）")

    print()
    print("=== 混叠检查：高音区泛音越界情况 ===")
    hit_any = False
    for name, hits in SETS.items():
        for f0, f1, f2, _ms, _g in hits:
            for b in (f0, f1, f2):
                if not b:
                    continue
                h3 = b * HARM_MUL3
                if h3 >= LIMIT:
                    hit_any = True
                    print("  {:10s} {}Hz 的 5.4 倍泛音 {:.0f}Hz 越界，已跳过"
                          .format(name, b, h3))
    if not hit_any:
        print("  无越界")


if __name__ == "__main__":
    main()
