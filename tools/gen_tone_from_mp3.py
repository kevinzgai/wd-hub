# -*- coding: utf-8 -*-
"""把用户的 MP3 素材转成固件可用的 PCM 表。

流程：
  1) 解码成 24kHz 单声道 int16
  2) 升调（重采样，默认 +12 半音 = 一个八度）
  3) 自动裁掉首尾静音
  4) 归一化 + 首尾短淡入淡出（避免咔声）
  5) 输出 C 头文件

为什么必须升调
--------------
原素材是"叮—咚"两声，实测基频：
    第 1 声 711Hz（清脆的"叮"）
    第 2 声 175Hz（低沉的"咚"）
175Hz 是钢琴 F3，板载小喇叭（约 1.5W、无共鸣腔）的频响下限在 300~400Hz，
这个音根本推不动 —— 实测高通 350Hz 后只剩 66% 能量，且靠谐波发声，
听感是一个闷闷的"咚"。这就是"听起来还是咚的一声"的真正原因，
不是固件没生效（采样与原始音频相关系数 1.0000，确实是同一段）。

升一个八度后：
    叮 711 -> 1422Hz  仍在小喇叭舒适区
    咚 175 ->  350Hz  刚好能被发出来
两个音都能还原，"叮咚"的听感才成立。

⚠️ 升调必须用**重采样**（改变播放速率），不能直接改频率乘数 ——
那样会破坏各次谐波的比例关系，听起来不自然（像变声器的失真）。
重采样是把整个波形在时间轴上压缩，所有频率等比例上升，谐波关系不变。

用法：
  python tools/gen_tone_from_mp3.py [源文件路径] [半音数]
  半音数默认 12（+1 八度）。0 = 不升调。
"""
import math
import os
import struct
import sys

import miniaudio

RATE = 24000
DEFAULT_SRC = r"C:\Users\Administrator\Downloads\307888piCBiw.mp3"
DEFAULT_SEMITONES = 12
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
HEADER_OUT = os.path.join(ROOT, "firmware", "main", "tone_pcm.h")
WAV_OUT = os.path.join(HERE, "done_tone.wav")


def decode(path, rate=RATE):
    dec = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16,
                                nchannels=1, sample_rate=rate)
    return list(dec.samples)


def pitch_shift(samples, semitones, rate=RATE):
    """升调：线性插值重采样。

    系数 = 2^(半音/12)。系数 >1 表示升调，输出会变短（时长 ÷ 系数），
    这是重采样的必然结果 —— 想要保持原时长就得补静音，这里不需要。

    用线性插值而不是最近邻：最近邻在 2 倍升调时等于丢弃一半样本，
    会产生明显的锯齿噪声（混叠）。线性插值便宜且足够 ——
    升调后最高频率也就 3kHz 左右，远低于 Nyquist。
    """
    if semitones == 0:
        return list(samples)

    step = 2.0 ** (semitones / 12.0)      # 频率乘数
    n_in = len(samples)
    n_out = int(n_in / step)
    out = []
    pos = 0.0
    for _ in range(n_out):
        i = int(pos)
        if i + 1 >= n_in:
            break
        frac = pos - i
        out.append(samples[i] * (1.0 - frac) + samples[i + 1] * frac)
        pos += step
    return out


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


def write_header(path, samples, semitones=0):
    ints = [int(max(-32768, min(32767, v))) for v in samples]
    lines = [
        "// 自动生成，勿手改。重新生成：python tools/gen_tone_from_mp3.py",
        "//",
        "// 提示音采样表。来源：用户提供的 307888piCBiw.mp3（\"叮咚\"铃声）。",
        "//",
        "// 处理过程：",
        "//   1) 裁掉首尾静音（原文件 2.09s，其中尾部静音 1.49s）",
    ]
    if semitones:
        ratio = 2.0 ** (semitones / 12.0)
        lines += [
            "//   2) 升调 +%d 半音（x%.3f），用重采样实现" % (semitones, ratio),
            "//      必须升调的原因：原素材第 2 声是 175Hz（钢琴 F3），",
            "//      板载小喇叭频响下限 300~400Hz，这个音推不动，",
            "//      听起来只剩一个闷闷的\"咚\"。升八度后 175->350Hz、",
            "//      711->1422Hz，两个音都进舒适区。",
        ]
    else:
        lines.append("//   2) 升调：未处理")
    lines += [
        "//   3) 归一化到满幅 96% + 首尾淡入淡出",
        "//",
        "// 最终 %d 帧 = %.0fms，采样率 %d Hz。" % (len(ints), len(ints) / RATE * 1000, RATE),
        "//",
        "// 采样率与 I2S 输出完全一致，运行时不需要重采样 ——",
        "// 只需把单声道复制到左右声道（I2S 配的是立体声，",
        "// 直接送单声道会让 L/R 错位，听起来是失真加噪声）。",
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
    semis = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SEMITONES
    if not os.path.isfile(src):
        print("源文件不存在:", src)
        return 1

    print("源文件:", src)
    samples = decode(src)
    print("  解码: %d 帧 = %.3fs @ %dHz" % (len(samples), len(samples) / RATE, RATE))

    # 先裁静音再升调：先裁能少处理 1.5 秒的静音数据，
    # 而且静音段对重采样没有意义，裁掉不影响结果。
    trimmed = trim_silence(samples)
    print("  裁静音: %d 帧 = %.3fs（裁掉 %.0fms）"
          % (len(trimmed), len(trimmed) / RATE,
             (len(samples) - len(trimmed)) / RATE * 1000))

    if semis:
        ratio = 2.0 ** (semis / 12.0)
        shifted = pitch_shift(trimmed, semis)
        print("  升调 +%d 半音（x%.3f）: %d 帧 = %.3fs"
              % (semis, ratio, len(shifted), len(shifted) / RATE))
    else:
        shifted = trimmed
        print("  升调: 不处理")

    final = polish(shifted)
    print("  归一化+淡入淡出: 峰值 %d" % max(abs(v) for v in final))

    write_wav(WAV_OUT, final)
    write_header(HEADER_OUT, final, semis)
    print()
    print("已写 %s" % HEADER_OUT)
    print("已写 %s（试听用）" % WAV_OUT)
    print("体积 %.1fKB" % (len(final) * 2 / 1024))

    # 升调后重新验证两个音的基频，确认都进了小喇叭频段
    print()
    print("=== 升调后基频验证 ===")
    verify_pitch(final, semis)
    return 0


def verify_pitch(samples, semis):
    """确认升调后两个音的基频都在小喇叭能还原的范围内。"""
    w = int(0.030 * RATE)

    def pitch_of(seg):
        m = sum(seg) / len(seg)
        seg = [v - m for v in seg]
        best_r, best_lag = 0.0, 0
        for lag in range(int(RATE / 4000), int(RATE / 150)):
            if lag >= len(seg):
                break
            num = sum(seg[i] * seg[i + lag] for i in range(len(seg) - lag))
            den = (sum(seg[i] ** 2 for i in range(len(seg) - lag)) *
                   sum(seg[i + lag] ** 2 for i in range(len(seg) - lag))) ** 0.5 + 1e-9
            r = num / den
            if r > best_r:
                best_r, best_lag = r, lag
        return (RATE / best_lag if best_lag else 0.0), best_r

    def rms(seg):
        return (sum(v * v for v in seg) / max(1, len(seg))) ** 0.5

    peak = max(abs(v) for v in samples) or 1
    # 前 110ms 视作第一个音，之后视作第二个音（原素材就是这个结构）
    split = int(0.110 * RATE)
    for label, seg in (("第 1 声", samples[:min(split, len(samples))]),
                       ("第 2 声", samples[split:split + w * 2])):
        if len(seg) < w or rms(seg) < peak * 0.03:
            continue
        seg = seg[:w]
        f, conf = pitch_of(seg)
        if f < 300:
            judge = "仍偏低，小喇叭吃力"
        elif f > 3000:
            judge = "偏高，可能刺耳"
        else:
            judge = "OK，在舒适区"
        print("  %s  基频 %6.1fHz  置信度 %.2f  %s" % (label, f, conf, judge))


if __name__ == "__main__":
    sys.exit(main())
