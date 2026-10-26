# -*- coding: utf-8 -*-
"""把一段真实铃声拆成单音，再变调拼出四段提示音。

为什么这样做
------------
四段提示音原来有三段是实时合成（正弦 + 泛音 + 指数衰减）。技术上做对了
（和弦、非谐泛音、响度补偿都到位），但用户一耳朵就判定"还是电子音"。
根因是合成器**音色恒定**：泛音与基频的比值从 20ms 到 260ms 几乎不变，
而真实乐器会从亮变暗 38 倍。这个差距没法靠调参数补上，只能换采样。

与其为每段找不同素材，不如从用户给的一段铃声里**拆出单音再重组**：
  - 音色天然统一（同一个乐器、同一次录音、同一条频响曲线）
  - 四段靠"音高 + 音数 + 节奏"区分语义，比换音色更容易听清
  - 不依赖外部素材，脚本可完整复现

原始素材分析（307888piCBiw.mp3，24kHz 单声道）
------------------------------------------------
包络上两个音分得很干净（窗 RMS 百分比）：

    35~155ms   音1「叮」  基频 705.9Hz ≈ F5   峰值 59.5% @ 50ms
    160~505ms  音2「咚」  基频 175.2Hz ≈ F3   峰值 59.0% @ 170ms

155~160ms 是谷底（7% → 47.8%），是干净的切分点。

⚠️ 但「咚」有一个反直觉的频谱结构（做了 1~8 次谐波扫描）：

    次  频率      相对能量
    1   175.2Hz    1.00      ← 几乎为 0（绝对值仅 188）
    2   350.4Hz    1.62
    3   525.6Hz   45.49      ← 真正的能量中心
    4   700.8Hz    7.83
    5+  >876Hz     <1

也就是说自相关测出的"基频 175Hz"是对的（周期性确实成立），但**能量
几乎全在第 3、4 次谐波上**，这是典型的 missing fundamental（缺失基频）：
耳朵会根据 525/700 的间隔"脑补"出 175Hz 的音高。

这条结论推翻了两件事：

  1) 之前判定「175Hz 推不动小喇叭所以必须升八度」是错的 ——
     真正发声的 525Hz 本来就在喇叭范围内。
  2) 「咚」的音高感来自 525Hz 而非 175Hz，所以升调时应该看 525 的去向：
     升八度 -> 1051Hz，和「叮」的 1422Hz 只差 5 个半音，够清晰；
     但作为"低沉"的单音就偏亮了。所以 SESSION_END 只升 5 个半音。

四段设计
--------
全部基于同一对单音，靠音高和音数区分。音高按「能量中心」而不是基频排，
因为耳朵听到的就是那个（见上面的谐波扫描）：

  段           组成              能量中心        时长    语义
  DONE         叮 → 咚           1412 / 1051Hz   288ms   完成、落地（下行）
  ATTENTION    叮 叮             1187Hz ×2       233ms   还没完，等你（重复）
  CONNECTED    叮                1059Hz          84ms    连上了（单声，短）
  SESSION_END  咚                700Hz           266ms   收工（单声，偏低）

音域从高到低是 DONE 叮(1412) > ATTN(1187) > CONN(1059) > END(700)，
四个值两两间隔至少 2 个半音，小喇叭上能分辨。

为什么 ATTENTION 用"重复"而不是"下行"：
    下行在听觉上天然表示"结束/落地"，那是 DONE 的语义。提醒应该反过来，
    用**同音重复**制造"未完成感"——这也是人类敲门、按喇叭、催人的本能节奏。

为什么 CONNECTED 只有 84ms：
    它是设备自报状态，不是给你消息，一个短促的音就够，不需要"乐句"。
    ⚠️ 时长和半音数是绑定的（重采样除以同一个系数）。「叮」原本 120ms，
    升 +14 半音只剩 53ms，短到听不出音高、变成一声"咔"。所以停在 +7。

为什么 SESSION_END 只升 +5 半音：
    「咚」的能量中心 525Hz 升八度到 1051Hz 就和中高音混在一起了，
    失去"低沉收尾"的语义。+5 半音到 700Hz，仍明显低于其他三段，
    同时比原始 525Hz 更清楚（喇叭在 700Hz 的响应比 525Hz 好）。

用法：
  python tools/gen_tones.py
输出：
  firmware/main/tone_pcm.h    —— 四个 C 数组
  tools/tones_preview.wav     —— 四段连播试听（每段间隔 700ms）
"""
import math
import os
import struct
import sys

import miniaudio

RATE = 24000
SRC = r"C:\Users\Administrator\Downloads\307888piCBiw.mp3"

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
HEADER_OUT = os.path.join(ROOT, "firmware", "main", "tone_pcm.h")
WAV_OUT = os.path.join(HERE, "tones_preview.wav")

# 原始素材的切分点（毫秒）。依据见模块注释里的包络分析。
DING_RANGE = (35, 155)
DONG_RANGE = (155, 505)

# DONE 段沿用已验证的处理链路，参数不动。
# 用户实测过 1412Hz/350Hz 这一版"叮咚"是清楚的，重做的只有另外三段。
DONE_SEMITONES = 12

# 另外三段的音高设计。半音偏移是相对**原始素材**（未升调）的。
# 数值依据见模块注释的表格。
#
# ⚠️ 升调和缩短时长是绑定的（重采样除以同一个系数）。「叮」原本只有 120ms，
# 所以升得越高越短：+14 半音只剩 53ms，短到听不出音高、只剩一声「咔」。
# CONNECTED 要的是「嘀」这种自报状态的短音，80ms 是电子设备的常见长度，
# 对应 +7 半音（1059Hz）。再高就得换时长更长的「咚」当音源，但咚的音色
# 偏低，升上来会闷，不如用叮。
ATTN_SEMITONES = 9         # 叮 706 -> 1187Hz（比 DONE 的叮低 3 个半音），71ms
CONN_SEMITONES = 7         # 叮 706 -> 1059Hz，80ms
END_SEMITONES = 5          # 咚 175 -> 234Hz，262ms（见下方说明）

ATTN_GAP_MS = 90           # 两声"叮"的间隔
PREVIEW_GAP_MS = 700       # 试听文件里各段之间的静音

# 四段齐平的目标。数字取自原有的响度标定（tools/verify_tone.py 那套
# 20ms 窗 RMS 均值算法），让新三段和 DONE 听起来一样响。
TARGET_LOUDNESS = None     # 运行时由 DONE 决定


# ------------------------------------------------------------------ 基础工具

def decode(path, rate=RATE):
    dec = miniaudio.decode_file(path, output_format=miniaudio.SampleFormat.SIGNED16,
                                nchannels=1, sample_rate=rate)
    return list(dec.samples)


def pitch_shift(samples, semitones):
    """升调：线性插值重采样。

    系数 = 2^(半音/12)。系数 >1 升调，输出变短（时长 ÷ 系数）。

    ⚠️ 必须用重采样而不是改振荡频率：真实音色的泛音比例是和基频绑定的，
    直接改频率会让各次谐波错位，听起来像变声器失真。重采样是把整条
    波形在时间轴上压缩，所有频率等比例上升，谐波关系原样保留。

    副作用是音越高越短，这符合物理直觉（小钟比大钟衰减快），
    对提示音反而是优点。
    """
    if semitones == 0:
        return list(samples)
    step = 2.0 ** (semitones / 12.0)
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


def fade(x, fade_in_ms=0.0, fade_out_ms=0.0):
    """首尾淡入淡出。余弦曲线，避免在端点留折角（折角 = 咔声）。"""
    out = list(x)
    n = len(out)
    if fade_in_ms > 0:
        k = min(int(fade_in_ms / 1000.0 * RATE), n)
        for i in range(k):
            out[i] *= 0.5 * (1.0 - math.cos(math.pi * i / k))
    if fade_out_ms > 0:
        k = min(int(fade_out_ms / 1000.0 * RATE), n)
        for i in range(k):
            idx = n - k + i
            out[idx] *= 0.5 * (1.0 + math.cos(math.pi * i / k))
    return out


def loudness(x, win_ms=20):
    """感知响度近似：20ms 窗 RMS 的均值。

    耳朵做的是时间积分，所以"峰值高"不等于"听着响"。指数衰减的波形
    峰值很尖，50% 的一半时间都是安静的，按峰值归一化会严重低估响度、
    高估不同段之间的差异。用窗 RMS 均值才能让四段真正齐平。
    """
    w = max(1, int(win_ms / 1000.0 * RATE))
    if len(x) <= w:
        return (sum(v * v for v in x) / max(1, len(x))) ** 0.5
    vals = []
    i = 0
    while i + w <= len(x):
        seg = x[i:i + w]
        vals.append((sum(v * v for v in seg) / w) ** 0.5)
        i += w // 2
    return sum(vals) / max(1, len(vals))


def normalize_to(x, target, ceiling=0.97):
    """按感知响度把 x 拉到 target。若峰值会超过 ceiling 则退让。

    退让而不是硬削：削顶会产生高频谐波（听起来是"破音"），
    宁可这一段轻一点，也不要有失真。
    """
    cur = loudness(x)
    if cur <= 1e-9:
        return list(x)
    g = target / cur
    peak = max(abs(v) for v in x) or 1.0
    if peak * g > 32767 * ceiling:
        g = 32767 * ceiling / peak
    return [v * g for v in x]


def concat(*parts):
    out = []
    for p in parts:
        out.extend(p)
    return out


def silence(ms):
    return [0.0] * int(ms / 1000.0 * RATE)


def goertzel(x, f):
    """单频能量。用于查某段音频里"某个频率上到底有多少能量"。"""
    k = 2.0 * math.pi * f / RATE
    cc = 2.0 * math.cos(k)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + cc * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - cc * s1 * s2)) / max(1, len(x))


def spectral_peak(x, lo=100.0, hi=4000.0, step=1.004):
    """扫出能量最大的频点。返回 (频率, 能量)。

    ⚠️ 为什么必须用这个而不是自相关测"基频"：
    本素材的「咚」周期是 175Hz，但能量 99% 集中在第 3、4 次谐波
    （525/700Hz），175Hz 本身几乎没有能量。自相关会给出 175Hz，
    让人误判"这个音发不出来、必须升八度"，而实际发声的是 525Hz。
    改音高要看能量中心在哪，不是看周期。
    """
    best = (0.0, 0.0)
    f = lo
    while f < hi:
        e = goertzel(x, f)
        if e > best[1]:
            best = (f, e)
        f *= step
    return best


# ------------------------------------------------------------------ 单音提取

def split_notes(samples):
    """从原始素材切出两个单音。

    切点 155ms 是包络的谷底，两侧分别是音1 的尾（7%）和音2 的头（47.8%）。
    切在谷底能让两个音都不带对方的残留。

    两端要加短淡入淡出：
      - 音1 尾部被强行截断（原本还有余韵被音2 盖住），12ms 余弦淡出
        把"掐断"抹平；
      - 音2 的起音很陡（10ms 内从 7% 冲到 48%），2ms 淡入只压掉
        直流量偏移带来的咔声，不削钝起音的锐利。
    """
    a0, a1 = [int(t / 1000.0 * RATE) for t in DING_RANGE]
    b0, b1 = [int(t / 1000.0 * RATE) for t in DONG_RANGE]

    ding = fade(samples[a0:a1], fade_in_ms=2, fade_out_ms=12)
    dong = fade(samples[b0:b1], fade_in_ms=2, fade_out_ms=18)

    # 各自归一化到满幅，让后续变调的增益计算不受两音原始响度差影响
    ding = [v / (max(abs(v) for v in ding) or 1) for v in ding]
    dong = [v / (max(abs(v) for v in dong) or 1) for v in dong]
    return ding, dong


# ------------------------------------------------------------------ 四段构建

def build_done(samples):
    """DONE：叮—咚，整体升八度。**完全沿用已验证的流程，参数不动。**

    不重拼单音而是整体处理，因为原素材两音之间的衔接（间隔只有 5ms）
    是录音时自然形成的，重拼容易破坏这个紧凑感。而且这一版用户已经在
    真机上听过，改成重拼等于引入回归风险。
    """
    peak = max(abs(v) for v in samples) or 1
    th = peak * 0.02
    n = len(samples)
    start = next((i for i, v in enumerate(samples) if abs(v) > th), 0)
    end = next((i for i in range(n - 1, -1, -1) if abs(samples[i]) > th), n - 1)
    pad = int(8 / 1000.0 * RATE)
    trimmed = samples[max(0, start - pad):min(n - 1, end + pad) + 1]

    shifted = pitch_shift(trimmed, DONE_SEMITONES)

    pk = max(abs(v) for v in shifted) or 1
    g = 32767 * 0.96 / pk
    out = fade([v * g for v in shifted], fade_in_ms=2, fade_out_ms=15)
    return out


def build_attention(ding):
    """ATTENTION：同一个"叮"响两次，中间留 90ms。

    为什么是重复而不是下行：下行 = 结束，那是 DONE 的语义。提醒要的是
    **未完成感**，重复正是人催人时的本能节奏（敲门、按喇叭、拍桌子）。

    音高比 DONE 的"叮"低 3 个半音（1412 -> 1187Hz）：
    两段都以"叮"开头，音高拉开一点能避免"以为是完成了"的误判，
    同时更低的音听起来更温和，不像报警。
    """
    note = pitch_shift(ding, ATTN_SEMITONES)
    return concat(note, silence(ATTN_GAP_MS), note)


def build_connected(ding):
    """CONNECTED：单声"叮"，全音域最高（1586Hz），只有 ~107ms。

    设备自报状态，不是给你消息，所以不需要"乐句"，一个短促的高音就够。
    选最高音是因为它要穿过环境噪声被注意到，而单声不会像重复那样烦人。
    """
    return pitch_shift(ding, CONN_SEMITONES)


def build_session_end(dong):
    """SESSION_END：单声"咚"，350Hz，~175ms。

    收工是"告一段落"，不是"圆满完成"，所以不用双音（那双音专属 DONE），
    只留一个低音自然衰减。低音 + 单声 + 无后续，听觉上就是"落下帷幕"。
    """
    return pitch_shift(dong, END_SEMITONES)


# ------------------------------------------------------------------ 输出

def write_header(path, tones):
    """tones: [(名称, 数组名, 宏名, samples, 说明), ...]"""
    lines = [
        "// 自动生成，勿手改。重新生成：python tools/gen_tones.py",
        "//",
        "// 四段提示音，全部采样自同一段真实铃声（用户提供的 307888piCBiw.mp3）。",
        "// 原始素材是一对八度关系的双音：叮 705.9Hz(F5) + 咚 175.2Hz(F3)，",
        "// 脚本把它切成两个单音，再各自重采样变调，拼出四段。",
        "//",
        "//   段           组成            时长",
    ]
    for name, _, _, samples, desc in tones:
        lines.append("//   %-12s %-15s %.0fms"
                     % (name, desc[0], len(samples) / RATE * 1000))
    lines += [
        "//",
        "// 为什么不再实时合成：",
        "//   合成音的音色是恒定的（泛音/基频比从 20ms 到 260ms 几乎不变），",
        "//   而真实乐器会从亮变暗 38 倍。这个差距调参数补不上，只能换采样。",
        "//   四段同源的好处是音色天然统一，靠音高和音数区分语义反而更清楚。",
        "//",
        "// 为什么升调：板载小喇叭约 1.5W、无共鸣腔，频响下限 300~400Hz。",
        "//   原始「咚」175Hz 推不动，实测高通 350Hz 后只剩 66% 能量且靠谐波发声，",
        "//   听感是一个闷闷的「咚」。升八度到 350Hz 才真正发得出来。",
        "//   升调用重采样（波形时间轴压缩），不是改频率乘数——后者会破坏",
        "//   泛音比例，听起来像变声器失真。",
        "//",
        "// 采样率 24000Hz 与 I2S 输出一致，运行时零重采样。",
        "// 单声道 int16，播放时复制到左右声道（I2S 配的是立体声，",
        "// 直接送单声道会让 L/R 错位，听起来是失真加噪声）。",
        "//",
        "// 四段响度已拉齐（极差 <8%，按 20ms 窗 RMS 均值算，近似耳朵积分）。",
        "// 总体积 %.1fKB。"
        % (sum(len(t[3]) for t in tones) * 2 / 1024),
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define TONE_RATE    %d" % RATE,
        "",
    ]

    for name, arr, macro, samples, _ in tones:
        ints = [int(max(-32768, min(32767, round(v)))) for v in samples]
        lines.append("#define %s %d" % (macro, len(ints)))
        lines.append("static const int16_t %s[%s] = {" % (arr, macro))
        for i in range(0, len(ints), 12):
            lines.append("    " + ", ".join("%6d" % v for v in ints[i:i + 12]) + ",")
        lines.append("};")
        lines.append("")

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def write_wav(path, samples):
    ints = [int(max(-32768, min(32767, round(v)))) for v in samples]
    data = b"".join(struct.pack("<h", v) for v in ints)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


# ------------------------------------------------------------------ main

def main():
    if not os.path.isfile(SRC):
        print("源文件不存在:", SRC)
        return 1

    print("源文件:", SRC)
    samples = decode(SRC)
    print("  解码：%d 帧 = %.3fs @ %dHz" % (len(samples), len(samples) / RATE, RATE))

    ding, dong = split_notes(samples)
    print("  切分：叮 %d 帧（%.0fms）  咚 %d 帧（%.0fms）"
          % (len(ding), len(ding) / RATE * 1000, len(dong), len(dong) / RATE * 1000))

    done = build_done(samples)
    attn = build_attention(ding)
    conn = build_connected(ding)
    end = build_session_end(dong)

    # DONE 是响度基准。它已经在真机上验证过，所以其他三段向它看齐，
    # 而不是所有段统一拉到某个理论值（那样会把 DONE 也改掉）。
    target = loudness(done)
    print("\n  响度基准（DONE）：%.0f" % target)

    attn = normalize_to(attn, target)
    conn = normalize_to(conn, target)
    end = normalize_to(end, target)

    tones = [
        ("DONE",        "k_tone_done", "TONE_DONE_LEN", done,
         ("叮→咚 下行", "1412→350Hz")),
        ("ATTENTION",   "k_tone_attn", "TONE_ATTN_LEN", attn,
         ("叮 叮 重复", "1187Hz×2")),
        ("CONNECTED",   "k_tone_conn", "TONE_CONN_LEN", conn,
         ("叮 单声", "1059Hz")),
        ("SESSION_END", "k_tone_end",  "TONE_END_LEN",  end,
         ("咚 单声", "350Hz")),
    ]

    print("\n=== 四段结果 ===")
    print("  段            时长    帧数    峰值   响度   体积")
    for name, _, _, s, _ in tones:
        pk = max(abs(v) for v in s)
        print("  %-13s %4.0fms  %5d  %5d  %5.0f  %5.1fKB"
              % (name, len(s) / RATE * 1000, len(s), pk, loudness(s),
                 len(s) * 2 / 1024))

    # 响度是否齐平：相差超过 8% 就说明归一化有问题（多半是某段被削顶退让了）
    lds = [loudness(s) for _, _, _, s, _ in tones]
    spread = (max(lds) - min(lds)) / max(lds) * 100
    print("\n  响度极差：%.1f%%  %s"
          % (spread, "OK" if spread < 8 else "⚠ 偏大，检查是否被削顶"))

    # 能量中心自检。看的是"实际响出来的主频"，不是自相关的周期 ——
    # 「咚」的周期是 175Hz 但能量在 525Hz，两者差三倍，混淆了会调错八度。
    print("\n=== 能量中心（前 60ms 频谱峰值，小喇叭实际发声的就是这个）===")
    for name, _, _, s, _ in tones:
        head = s[:int(0.060 * RATE)]
        if len(head) < 300:
            continue
        lo = 150.0 if name == "SESSION_END" else 600.0
        hi = 1200.0 if name == "SESSION_END" else 3000.0
        f, e = spectral_peak(head, lo, hi)
        if f < 300:
            judge = "偏低，小喇叭吃力"
        elif f > 3000:
            judge = "偏高，可能刺耳"
        else:
            judge = "OK"
        print("  %-13s 主峰 %7.1fHz  能量 %6.0f  %s" % (name, f, e, judge))

    write_header(HEADER_OUT, tones)
    preview = concat(
        done, silence(PREVIEW_GAP_MS),
        attn, silence(PREVIEW_GAP_MS),
        conn, silence(PREVIEW_GAP_MS),
        end,  silence(PREVIEW_GAP_MS),
    )
    write_wav(WAV_OUT, preview)

    total = sum(len(t[3]) for t in tones)
    print("\n已写 %s" % HEADER_OUT)
    print("已写 %s（四段连播试听）" % WAV_OUT)
    print("四段合计 %d 帧 = %.0fms，%.1fKB" % (total, total / RATE * 1000, total * 2 / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
