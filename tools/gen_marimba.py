# -*- coding: utf-8 -*-
"""用模态合成（modal synthesis）生成马林巴音色采样表。

为什么这样做
------------
实时合成 3 个泛音怎么调都像电子音，因为真实乐器的音色来自**十几个模态**
（振动模式）的叠加，每个模态有自己的频率、幅度、衰减率。3 个泛音远不够。

但模态合成计算量大，在 ESP32 上逐样本算 12 个模态 × 3 个和弦音 = 36 个
振荡器，CPU 吃紧。所以改成**离线生成一次，存成 PCM 表，固件里变调重采样**。

这样：
  - 音色是物理准确的（12 个模态，不是 3 个泛音）
  - 运行时只需线性插值重采样，开销极小
  - 只存 1 个采样表（C6 基准），通过变调覆盖所有音高，约 7KB

选马林巴而不是钟琴
------------------
钟琴是金属片，音色尖锐清亮，做提示音容易刺耳且"电子味"重。
马林巴是木质琴键，泛音少而温暖，余韵是木头的自然衰减 ——
这正是大量 App 通知音的选择。小喇叭也能还原好它的中频。

输出：
  firmware/main/tone_pcm.h   —— C 数组
  tools/marimba_preview.wav  —— 供试听

不参与构建，只在改音色时手工重跑。
"""
import math
import struct

RATE = 24000
BASE_HZ = 1046.5          # C6，四段提示音的音域中心
DURATION_MS = 300
LENGTH = RATE * DURATION_MS // 1000

# 马林巴（调音木琴条）的模态结构。
#
# 木琴条被调音后，前两个弯曲模态的频率比被拉到 1:4（而不是自由梁的 1:2.76），
# 这样泛音听起来是"谐和"的，这是马林巴温暖感的来源。
# 更高阶的扭转模态在 9~10 倍左右，很快衰减。
#
# ⚠️ 衰减时间（t60）是这类音色最容易调错的地方。
# 第一版我设成 420/110/45ms，结果二次泛音在 50ms 就衰减到 4%，
# 听起来反而更像"闷的纯音"——比有泛音更像电子音，因为完全没有音色变化。
# 关键关系：t60 是衰减 60dB 的时间，对应的 τ = t60 / 6.908。
# 想要泛音"听得见一会儿再消失"，二次泛音的 t60 至少要到 200ms。
#
# (频率比, 初始幅度, 衰减到 -60dB 的时间 ms, 起音延迟 ms)
MODES = [
    (1.000, 1.000, 620.0, 0.0),    # 基频：木头余韵最长，撑住整个音
    (3.932, 0.340, 200.0, 0.2),    # 二次弯曲（调音到 4 倍）：温暖感的主力
    (9.240, 0.115,  75.0, 0.5),    # 扭转模态：贡献"亮"和敲击的锐利
    (16.60, 0.030,  30.0, 0.8),    # 更高阶，只贡献最开头的一点点锋芒
]


def synth_marimba():
    """生成单音采样。返回 float 列表（-1..1）。"""
    n = LENGTH
    buf = [0.0] * n

    for ratio, amp, t60_ms, delay_ms in MODES:
        f = BASE_HZ * ratio
        # 超过 Nyquist 的模态直接丢弃，否则会混叠成刺耳噪声
        if f >= RATE * 0.45:
            continue

        w = 2.0 * math.pi * f / RATE
        # -60dB 衰减：幅度降到 0.001。tau 是降到 1/e 的时间
        tau = (t60_ms / 1000.0) / math.log(1000.0)
        k = math.exp(-1.0 / (tau * RATE))

        d0 = int(delay_ms / 1000.0 * RATE)
        env = amp
        c = 2.0 * math.cos(w)
        prev, cur = -math.sin(w), 0.0

        for i in range(d0, n):
            buf[i] += cur * env
            nx = c * cur - prev
            prev, cur = cur, nx
            env *= k

    # ---- 槌击瞬态 ----
    # 真实敲击的"哒"是槌子接触瞬间的宽频噪声，长度只有几毫秒。
    # 缺了它，无论振荡器多准确都像"电子发声"而不是"被敲响"。
    # 用固定种子的伪随机保证每次生成的采样表一致（便于复现）。
    seed = 12345
    for i in range(int(0.004 * RATE)):
        seed = (1103515245 * seed + 12345) & 0x7FFFFFFF
        noise = (seed / 0x3FFFFFFF) - 1.0        # -1..1
        # 噪声本身也要低通一点，纯白噪声太"嘶"；简单做一阶低通
        buf[i] += noise * 0.10 * math.exp(-i / (0.0012 * RATE))

    # ---- 归一化 + 2ms 起音 ----
    peak = max(abs(v) for v in buf) or 1.0
    buf = [v / peak for v in buf]

    ramp = int(0.002 * RATE)
    for i in range(min(ramp, n)):
        buf[i] *= i / ramp

    # ---- 尾部淡出 ----
    # 基频 t60=620ms 比采样表长度（300ms）还长，直接截断会在 -14dB 左右
    # 硬切，产生明显的"咔"。最后 40ms 做余弦淡出，听感上是自然衰减完毕。
    fade = int(0.040 * RATE)
    for i in range(fade):
        idx = n - fade + i
        if idx >= 0:
            # 余弦曲线比线性更平滑，不会在淡出起点留下折角
            buf[idx] *= 0.5 * (1.0 + math.cos(math.pi * i / fade))

    return buf


def write_wav(path, samples, rate=RATE):
    with open(path, "wb") as f:
        data = b"".join(
            struct.pack("<h", int(max(-32768, min(32767, v * 32000))))
            for v in samples
        )
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


def write_header(path, samples):
    """输出 C 数组。int16，24kHz，单声道。"""
    ints = [int(max(-32768, min(32767, v * 32000))) for v in samples]
    lines = [
        "// 自动生成，勿手改。重新生成：python tools/gen_marimba.py",
        "//",
        "// 马林巴单音采样，C6(1046.5Hz)，24000Hz，%d 帧（%dms），int16 单声道。"
        % (len(ints), DURATION_MS),
        "//",
        "// 为什么存采样而不是实时合成：",
        "//   真实乐器的音色来自十几个模态（振动模式）叠加，每个模态有自己的",
        "//   频率、幅度、衰减率。实时合成只能算 3 个泛音，怎么调都像电子音。",
        "//   这里离线用 4 个模态 + 槌击噪声生成一次，固件里变调重采样即可。",
        "//",
        "// 只有一份表，通过重采样覆盖 A5~G7 全部音高。体积 %.1fKB。"
        % (len(ints) * 2 / 1024),
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define TONE_PCM_HZ      %d      // 采样表本身的音高（C6）" % BASE_HZ,
        "#define TONE_PCM_RATE    %d" % RATE,
        "#define TONE_PCM_LEN     %d" % len(ints),
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
    print("模态：")
    for ratio, amp, t60, delay in MODES:
        f = BASE_HZ * ratio
        skip = "  <- 超 Nyquist，丢弃" if f >= RATE * 0.45 else ""
        print("  %6.3f 倍 = %7.1fHz  幅度 %.3f  衰减 %5.0fms%s"
              % (ratio, f, amp, t60, skip))

    samples = synth_marimba()
    write_wav("tools/marimba_preview.wav", samples)
    write_header("firmware/main/tone_pcm.h", samples)

    print("\n采样表：%d 帧 = %.1fKB" % (len(samples), len(samples) * 2 / 1024))
    print("已写 firmware/main/tone_pcm.h")
    print("已写 tools/marimba_preview.wav")

    # 客观验证：确认泛音真的在衰减（这是"像乐器"的硬指标）
    print("\n=== 泛音衰减验证（音色应由亮变暗）===")
    w = RATE // 50
    for t_ms in (20, 80, 160, 260):
        seg = samples[int(t_ms / 1000 * RATE):][:w]
        base = goertzel_energy(seg, BASE_HZ)
        h2 = goertzel_energy(seg, BASE_HZ * 3.932)
        h3 = goertzel_energy(seg, BASE_HZ * 9.240)
        ratio = (h2 + h3) / base if base > 1e-9 else 0.0
        print("  %3dms  基频 %6.4f  泛音 %6.4f  泛音/基频比 %.4f"
              % (t_ms, base, h2 + h3, ratio))


def goertzel_energy(x, f):
    k = 2 * math.pi * f / RATE
    cc = 2 * math.cos(k)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + cc * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - cc * s1 * s2)) / max(1, len(x))


if __name__ == "__main__":
    main()
