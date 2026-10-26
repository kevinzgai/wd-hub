# -*- coding: utf-8 -*-
"""把用户的 MP3 素材转成固件可用的 PCM 表。

流程：
  1) 解码成 24kHz 单声道 int16（与 I2S 输出采样率一致，运行时不用重采样）
  2) 自动裁掉首尾静音
  3) 归一化 + 首尾短淡入淡出（避免咔声）
  4) 输出 C 头文件

设计决定：**不做变调、不做压缩**。
素材本身是 0.56 秒的"叮咚"铃声，能量集中在 440~800Hz，
正好在小喇叭能还原的频段（200Hz 以下仅占 1.7%）。
既然用户明确指定要这个声音，就不要自作聪明改它的音色，
唯一该做的是裁掉那 1.53 秒静音 —— 否则每次响完要等 1.5 秒静默，
连续响两声时会觉得设备卡住了。

用法：
  python tools/gen_tone_from_mp3.py [源文件路径]
"""
import math
import os
import struct
import sys

import miniaudio

RATE = 24000
DEFAULT_SRC = r"C:\Users\Administrator\Downloads\307888piCBiw.mp3"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
HEADER_OUT = os.path.join(ROOT, "firmware", "main", "tone_pcm.h")
WAV_OUT = os.path.join(HERE, "done_tone.wav")


def decode(path):
    dec = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16,
                                nchannels=1, sample_rate=RATE)
    return list(dec.samples)


def trim_silence(samples, thresh_ratio=0.02, pad_ms=8):
    """裁掉首尾静音，两端各留 pad_ms 余量。

    留余量而不是精确切在第一个样本上：切太紧会把声音的起音（attack）
    切掉一点，听感上变成"被掐住"。8ms 足够安全又几乎不增加长度。
    """
    peak = max(abs(v) for v in samples) or 1
    th = peak * thresh_ratio
    n = len(samples)
    start = next((i for i, v in enumerate(samples) if abs(v) > th), 0)
    end = next((i for i in range(n - 1, -1, -1) if abs(samples[i]) > th), n - 1)

    pad = int(pad_ms / 1000.0 * RATE)
    start = max(0, start - pad)
    end = min(n - 1, end + pad)
    return samples[start:end + 1]


def polish(samples, fade_in_ms=2, fade_out_ms=15):
    """归一化 + 首尾淡入淡出。

    归一化到满幅的 96%：留一点余量，因为固件里还会乘音量百分比，
    满幅归一配 100% 音量会削顶。
    """
    peak = max(abs(v) for v in samples) or 1
    target = 32767 * 0.96
    g = target / peak
    out = [v * g for v in samples]

    fi = min(int(fade_in_ms / 1000.0 * RATE), len(out))
    for i in range(fi):
        out[i] *= i / fi
    fo = min(int(fade_out_ms / 1000.0 * RATE), len(out))
    for i in range(fo):
        idx = len(out) - fo + i
        # 余弦曲线比线性平滑，不会在起点留折角
        out[idx] *= 0.5 * (1.0 + math.cos(math.pi * i / fo))
    return out


def write_wav(path, samples):
    data = b"".join(
        struct.pack("<h", int(max(-32768, min(32767, v)))) for v in samples
    )
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


def write_header(path, samples):
    ints = [int(max(-32768, min(32767, v))) for v in samples]
    lines = [
        "// 自动生成，勿手改。重新生成：python tools/gen_tone_from_mp3.py",
        "//",
        "// 提示音采样表。来源：用户提供的 307888piCBiw.mp3（\"叮咚\"铃声）。",
        "// 已裁掉首尾静音（原文件 2.09s，其中尾部静音 1.49s），",
        "// 保留有效发声段 %d 帧 = %.0fms。" % (len(ints), len(ints) / RATE * 1000),
        "//",
        "// 采样率 %d Hz —— 与 I2S 输出采样率完全一致，运行时不需要重采样，" % RATE,
        "// 直接 memcpy 进发送缓冲，CPU 开销为零。",
        "//",
        "// 单声道 %d 帧，int16。体积 %.1fKB。" % (len(ints), len(ints) * 2 / 1024),
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define TONE_PCM_LEN     %d" % len(ints),
        "#define TONE_PCM_RATE    %d" % RATE,
        "",
        "static const int16_t k_tone_pcm[TONE_PCM_LEN] = {",
    ]
    for i in range(0, len(ints), 12):
        chunk = ints[i:i + 12]
        lines.append("    " + ", ".join("%6d" % v for v in chunk) + ",")
    lines.append("};")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src):
        print("源文件不存在:", src)
        return 1

    print("源文件:", src)
    samples = decode(src)
    print("  解码: %d 帧 = %.3fs @ %dHz" % (len(samples), len(samples) / RATE, RATE))

    trimmed = trim_silence(samples)
    print("  裁静音: %d 帧 = %.3fs（裁掉 %.0fms）"
          % (len(trimmed), len(trimmed) / RATE,
             (len(samples) - len(trimmed)) / RATE * 1000))

    final = polish(trimmed)
    print("  归一化+淡入淡出: 峰值 %d" % max(abs(v) for v in final))

    write_wav(WAV_OUT, final)
    write_header(HEADER_OUT, final)
    print()
    print("已写 %s" % HEADER_OUT)
    print("已写 %s（试听用）" % WAV_OUT)
    print("体积 %.1fKB" % (len(final) * 2 / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
