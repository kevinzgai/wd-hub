# -*- coding: utf-8 -*-
"""分析用户提供的提示音素材，判断能否用于 ESP32 固件。

分析目标：
  1) 有效声音区间（去掉首尾静音）—— 2.09s 对提示音太长
  2) 主频分布 —— 小喇叭（1.5W 左右）在 200Hz 以下基本出不来
  3) 是否需要裁短 / 变调 / 重采样

结论会写进固件生成脚本的注释。
"""
import os
import sys

import miniaudio

SRC = r"C:\Users\Administrator\Downloads\307888piCBiw.mp3"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "main")


def decode(path, rate=24000):
    dec = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16,
                                nchannels=1, sample_rate=rate)
    return list(dec.samples), dec.sample_rate


def find_active(samples, rate, thresh_ratio=0.02):
    """找有效声音区间。阈值取峰值的 2% —— 低于这个基本是噪声底。"""
    peak = max(abs(v) for v in samples) or 1
    th = peak * thresh_ratio
    start = 0
    for i, v in enumerate(samples):
        if abs(v) > th:
            start = i
            break
    end = len(samples) - 1
    for i in range(len(samples) - 1, -1, -1):
        if abs(samples[i]) > th:
            end = i
            break
    return start, end


def goertzel(x, f, rate):
    k = 2 * 3.141592653589793 * f / rate
    cc = 2 * 2.0 * __import__("math").cos(k) / 2
    import math
    cc = 2 * math.cos(k)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + cc * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - cc * s1 * s2)) / max(1, len(x))


def main():
    if not os.path.isfile(SRC):
        print("源文件不存在:", SRC)
        return 1

    samples, rate = decode(SRC)
    peak = max(abs(v) for v in samples)
    print("=== 素材概况 ===")
    print("  采样率 %d Hz  帧数 %d  时长 %.3f 秒  峰值 %d"
          % (rate, len(samples), len(samples) / rate, peak))

    start, end = find_active(samples, rate)
    print()
    print("=== 有效区间（去掉首尾静音）===")
    print("  起点 %.3fs  终点 %.3fs  有效时长 %.3fs"
          % (start / rate, end / rate, (end - start + 1) / rate))
    print("  首部静音 %.0fms  尾部静音 %.0fms"
          % (start / rate * 1000, (len(samples) - 1 - end) / rate * 1000))

    # 包络：按 20ms 分段看音量走向，判断是"一下"还是"多下"
    print()
    print("=== 音量包络（20ms 分段，看有几段声音）===")
    w = rate // 50
    bars = []
    for i in range(0, len(samples) - w, w):
        seg = samples[i:i + w]
        rms = (sum(v * v for v in seg) / w) ** 0.5
        bars.append((i / rate * 1000, rms))
    mx = max(b for _, b in bars) or 1
    # 只打印 RMS 超过峰值 8% 的段，压缩输出
    active = [(t, b) for t, b in bars if b > mx * 0.08]
    print("  有声段共 %d 个 20ms 窗口" % len(active))
    for t, b in active[::2]:     # 隔一个打印一个
        n = int(b / mx * 40)
        print("  %6.0fms |%s" % (t, "#" * n))

    # 主频分析
    print()
    print("=== 主频能量分布（判断小喇叭能否还原）===")
    seg = samples[start:start + min(len(samples) - start, rate // 4)]
    import math
    energies = []
    for f in (100, 200, 300, 440, 600, 800, 1000, 1200, 1500, 1800,
              2200, 2600, 3100, 3700, 4400, 5200, 6200, 7400, 8800, 10500):
        e = goertzel(seg, f, rate)
        energies.append((f, e))
    me = max(e for _, e in energies) or 1
    low = 0.0
    for f, e in energies:
        n = int(e / me * 40)
        mark = ""
        if f < 200:
            mark = "  <- 小喇叭基本出不来"
            low += e
        print("  %5dHz |%s%s" % (f, "#" * n, mark))
    print("  200Hz 以下能量占比 %.1f%%" % (low / sum(e for _, e in energies) * 100))

    print()
    print("=== 处理建议 ===")
    dur = (end - start + 1) / rate
    if dur > 0.8:
        print("  有效时长 %.2fs 偏长（提示音建议 <=0.6s），需要裁短或加速" % dur)
    else:
        print("  有效时长 %.2fs，适合直接使用" % dur)
    return 0


if __name__ == "__main__":
    sys.exit(main())
