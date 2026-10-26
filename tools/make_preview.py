# -*- coding: utf-8 -*-
"""生成提示音试听文件，用于人工判断音色。

三组对比，每组间隔 700ms：
  1) v2 实时合成（当前固件，用户反馈"还是像电子音"）
  2) 马林巴采样（新方案）

每组都按完整的四段提示音播放：连接 -> 完成 -> 收工 -> 等待。

用法：python tools/make_preview.py
输出：tools/tone_compare.wav
"""
import math
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_marimba
from verify_tone import synth as synth_v2, SETS

RATE = 24000


def resample(table, base_hz, target_hz, out_frames):
    """变调重采样：把基准音高的采样表拉到目标音高。

    线性插值就够 —— 变调范围不超过一个八度，插值误差远小于
    喇叭本身的失真。固件里也是同一套算法，所以试听和实机一致。
    """
    step = base_hz / target_hz
    out = []
    n = len(table)
    pos = 0.0
    for _ in range(out_frames):
        i = int(pos)
        if i + 1 >= n:
            out.append(0.0)
            pos += step
            continue
        frac = pos - i
        out.append(table[i] * (1.0 - frac) + table[i + 1] * frac)
        pos += step
    return out


def place(dst, src, at_ms, gain=1.0):
    """把 src 混入 dst 的 at_ms 位置。"""
    at = int(at_ms / 1000.0 * RATE)
    for i, v in enumerate(src):
        idx = at + i
        if idx >= len(dst):
            break
        dst[idx] += v * gain


def marimba_note(table, hz, dur_ms, gain=1.0):
    """用采样表生成一个音（含变调和时长裁剪）。"""
    frames = int(dur_ms / 1000.0 * RATE)
    # 多生成一点让尾部淡出自然
    out = resample(table, gen_marimba.BASE_HZ, hz, min(frames, len(table)))
    fade = min(int(0.02 * RATE), len(out))
    for i in range(fade):
        idx = len(out) - fade + i
        out[idx] *= 0.5 * (1.0 + math.cos(math.pi * i / fade))
    return [v * gain for v in out]


def build_marimba_set(table):
    """用马林巴采样重编四段提示音。

    和声进行保持不变（V-I 终止、小调、下行三度），只换音色。
    这样能公平对比"音色"这一个变量。
    """
    total_ms = 1400
    buf = [0.0] * int(total_ms / 1000.0 * RATE)

    # 均匀归一化音量。采样表已经峰值归一到 1.0，
    # 但和弦叠加会超，所以每个音先乘 0.42（约 1/2.4）。
    g = 0.42

    # --- CONNECTED: C7 -> G7 ---
    t = 0
    place(buf, marimba_note(table, 2093, 60, g), t); t += 110
    place(buf, marimba_note(table, 3136, 70, g), t); t += 400

    # --- DONE: G大三 -> C大三 ---
    # 和弦用"三个音分别合成后叠加"，比同时生成更自然（真实演奏也有微小时间差）
    def chord(hz_list, dur, at, gg=g):
        for hz in hz_list:
            place(buf, marimba_note(table, hz, dur, gg), at)

    base = t
    chord([784, 988, 1175], 150, base)
    chord([1047, 1319, 1568], 300, base + 165)
    t = base + 520

    # --- SESSION_END: Am ---
    chord([880, 1047, 1319], 260, t)
    t += 420

    # --- ATTENTION: E6+G6 -> A5+C6 ---
    chord([1319, 1568], 120, t)
    chord([880, 1047], 210, t + 150)
    t += 420

    return buf, t


def build_v2_set():
    """当前固件的四段（实时合成版）。"""
    total_ms = 1400
    buf = [0.0] * int(total_ms / 1000.0 * RATE)
    t = 0
    for name in ("CONNECTED", "DONE", "SESSION", "ATTENTION"):
        out, _ = synth_v2(SETS[name])
        place(buf, [v / 32000.0 for v in out], t)
        t += len(out) / RATE * 1000 + 350
    return buf, t


def write_wav(path, samples, rate=RATE):
    data = b"".join(
        struct.pack("<h", int(max(-32768, min(32767, v * 32000))))
        for v in samples
    )
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


def main():
    table = gen_marimba.synth_marimba()

    print("生成对比音频...")
    v2, v2_end = build_v2_set()
    print("  1) v2 实时合成   时长 %.2fs" % (v2_end / 1000))

    mar, mar_end = build_marimba_set(table)
    print("  2) 马林巴采样     时长 %.2fs  （体积 %d 字节）"
          % (mar_end / 1000, len(table) * 2))

    # 拼接：v2 / 间隔 / marimba
    gap = [0.0] * int(0.7 * RATE)
    allpcm = v2 + gap + mar
    write_wav("tools/tone_compare.wav", allpcm)
    print("\n已写 tools/tone_compare.wav")
    print("  顺序：v2 实时合成 -> 0.7s 静音 -> 马林巴采样")

    # 顺便统计峰值，确认没削顶
    peak = max(abs(v) for v in allpcm)
    print("  峰值 %.3f（1.0 为满幅，超过会削顶）" % peak)


if __name__ == "__main__":
    main()
