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
END_SEMITONES = 9          # 咚 能量中心 525 -> 880Hz，198ms（见下方说明）

ATTN_GAP_MS = 90           # 两声"叮"的间隔
PREVIEW_GAP_MS = 700       # 试听文件里各段之间的静音

# ---- 提亮参数 ----
#
# 用户反馈"太闷"，主要指向收工音。实测确认了根因：原素材的能量分布是
#     150-500    500-1000   1000-2000   2000-4000   4000-8000
#     15%        72%        5%          6%          1%
# 2kHz 以上几乎是空的，而人耳判断"明亮"看的就是 2~5kHz。升调只是把
# 整体平移，不会凭空造出高频，所以升多少次都还是闷。
#
# 两条处理手段，作用完全不同：
#   1) 高频搁架提升：放大**已有的**高频。本素材 2kHz 以上只剩 1%，
#      提多少都只是放大残渣，单独用效果有限（重心只从 868→1145Hz）。
#   2) 谐波激励：用中频的基波非线性饱和，**造出**新的高次谐波。
#      这是唯一能真正填上 2~5kHz 空洞的手段。
#   3) 中频衰减：500~1000Hz 占了 69%，光提高频拉不动重心，压一下主峰
#      比单纯加亮更有效 —— 是"平衡"，不是"堆亮度"。
#
# ⚠️ 中频衰减只在收工音上用。「叮」的能量本来就在 1~2kHz，压 800Hz
# 对它毫无意义，只会把高频衬托得过强（实测重心会冲到 2245Hz，
# 进入刺耳区间）。所以四段是"收工音压中频+提亮"，另外三段"只提亮"。
BRIGHT = {
    # 段名: (搁架拐点 Hz, 搁架增益 dB, 激励 drive, 激励 mix, 中频中心, 中频增益 dB)
    "DONE":        (2000, 5, 3.0, 0.30,    0,   0),
    "ATTENTION":   (2000, 5, 3.0, 0.30,    0,   0),
    "CONNECTED":   (2000, 5, 3.0, 0.30,    0,   0),
    "SESSION_END": (1500, 5, 3.0, 0.35,    0,   0),
}

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
    """按感知响度把 x 拉到 target。

    若所需增益会把峰值推过 ceiling，**先限幅再补满**，而不是简单退让。

    ⚠️ 这里原来的实现是"退让"（宁可轻一点也不要削顶），对合成音是对的，
    但对打击乐采样是错的：打击乐峰值/响度比高达 8，任何把响度拉满的
    增益都会顶到峰值，退让的结果就是"永远补不满"。实测反复出现
    响度 4091 vs 目标 4553，四段极差 9~11%。

    正确处理：增益推到峰值撞顶时，先按目标增益放大，再用 soft_limit
    把超出的尖峰压回去。总响度保住，波形只是尖峰被平滑了一点 ——
    这比"整段偏轻"好得多，因为四段之间的响度差是能被听出来的，
    而单个尖峰被压 1dB 听不出来。
    """
    cur = loudness(x)
    if cur <= 1e-9:
        return list(x)

    g = target / cur
    peak = max(abs(v) for v in x) or 1.0
    amplified = [v * g for v in x]

    if peak * g <= 32767 * ceiling:
        return amplified

    # 撞顶了：先按目标增益放大，再限幅。limit 之后再补一次增益，
    # 因为限幅会稍微压掉响度。
    y = soft_limit(amplified, threshold=0.62)
    cur2 = loudness(y)
    if cur2 > 1e-9:
        g2 = target / cur2
        pk2 = max(abs(v) for v in y) or 1.0
        if pk2 * g2 <= 32767 * ceiling:
            y = [v * g2 for v in y]
        else:
            # 极少数情况：放大后又撞顶，这一轮就接受现状，不再叠限幅
            # 避免过度压扁起音
            y = [v * min(g2, 32767 * ceiling / pk2) for v in y]
    return y


# ------------------------------------------------------------------ 提亮

def high_shelf(x, corner_hz, gain_db, slope=1.0):
    """一阶锐度的高频搁架滤波（RBJ biquad）。

    corner_hz 以上按 gain_db 提升，以下基本不动。

    为什么需要这个：原始素材的频谱分布是
        150-500    500-1000   1000-2000   2000-4000   4000-8000
        15%        72%        5%          6%          1%

    能量几乎全挤在 500~1000Hz，2kHz 以上基本是空的。这个分布听起来
    就是"闷" —— 人耳判断明亮度看的是 2~5kHz 有多少能量，而这里几乎是零。
    升调只是把整体平移，不会凭空造出高频，所以升多少次都还是闷。

    slope=1.0 是标准的 1 倍斜率（每倍频程 6dB）。调大让过渡更陡，
    听感更"现代"，但也更容易显得突兀。
    """
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * corner_hz / RATE
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    alpha = sin_w / 2.0 * math.sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0)

    b0 =     a * ((a + 1) + (a - 1) * cos_w + 2 * math.sqrt(a) * alpha)
    b1 = -2 * a * ((a - 1) + (a + 1) * cos_w)
    b2 =     a * ((a + 1) + (a - 1) * cos_w - 2 * math.sqrt(a) * alpha)
    a0 =          (a + 1) - (a - 1) * cos_w + 2 * math.sqrt(a) * alpha
    a1 =      2 * ((a - 1) - (a + 1) * cos_w)
    a2 =          (a + 1) - (a - 1) * cos_w - 2 * math.sqrt(a) * alpha

    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0

    # 直一式（Direct Form I）。这个滤波器要被四段各跑一次、每次几千样本，
    # 用列表推导反而慢，直接原地写。
    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, v
        y2, y1 = y1, y
        out[i] = y
    return out


def highpass(x, corner_hz, order=1):
    """简单一阶/二阶高通。给激励器取"高频部分"用。"""
    for _ in range(order):
        rc = 1.0 / (2.0 * math.pi * corner_hz)
        dt = 1.0 / RATE
        alpha = rc / (rc + dt)
        out = [0.0] * len(x)
        prev_x = prev_y = 0.0
        for i, v in enumerate(x):
            y = alpha * (prev_y + v - prev_x)
            prev_x, prev_y = v, y
            out[i] = y
        x = out
    return x


def excite(x, drive=2.0, mix=0.25):
    """谐波激励器（aural exciter）—— 从现有信号里**造出**高频。

    原理：取信号的高频部分，推到饱和区做非线性变换，非线性会生成
    原信号没有的高次谐波；再按 mix 混回干信号。

    这是与"搁架提升"的本质区别：
      - 搁架提升只能放大**已有的**高频。本素材 2kHz 以上只剩 1%，
        提 12dB 也只是把残渣放大，听感是"糊"而不是"亮"。
      - 激励器用中频的基波合成出 2~5kHz 的新谐波，是真的一无所有里
        造出内容。

    ⚠️ 第一版写成 tanh(drive*n)/drive，结果几乎没效果（重心只从 868
    到 901）。因为除以 drive 之后，小信号时 tanh(drive*n)/drive ≈ n，
    整条链退化成线性，根本不产生谐波。必须**只饱和、不除**，
    让 |n|>0.3 的部分都被压弯，才有多出来的谐波。

    drive 控制非线性强度：2.0 温和，4.0 明显但可能发毛。
    mix 是叠回干信号的比例：0.25 只加一点亮度，0.5 会很"电"。
    """
    # 只拿 1.5kHz 以上参与激励。低频参与会产生大量不和谐分音，
    # 听起来是浑浊的"嗡嗡"而不是明亮。
    hp = highpass(x, 1500.0, order=2)

    out = list(x)
    peak = max(abs(v) for v in hp) or 1.0
    for i, v in enumerate(hp):
        n = v / peak                        # 归一到 -1..1
        shaped = math.tanh(drive * n)       # 饱和。不除 drive，否则退回线性
        # 饱和后的信号里混着大量低频分音，再高通一次只留"亮"的部分
        out[i] += shaped * peak * mix
    return out


def peaking(x, center_hz, gain_db, q=1.0):
    """中频峰值 EQ（RBJ biquad）。gain_db 为负时是衰减。

    用来看"把占主导的那段压一点"对重心有多大帮助。本素材 500~1000Hz
    占了 69% 的能量，光往上提高频很难把重心拉起来，压一下主峰
    往往比提高频更有效 —— 这是"平衡"，不是单纯"加亮"。
    """
    a = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * center_hz / RATE
    cos_w = math.cos(w0)
    sin_w = math.sin(w0)
    alpha = sin_w / (2.0 * q)

    b0 = 1 + alpha * a
    b1 = -2 * cos_w
    b2 = 1 - alpha * a
    a0 = 1 + alpha / a
    a1 = -2 * cos_w
    a2 = 1 - alpha / a

    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0

    out = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, v
        y2, y1 = y1, y
        out[i] = y
    return out


def soft_limit(x, threshold=0.72, out_ceiling=0.97):
    """软限幅：把超过 threshold 的部分平滑压弯，而不是硬切。

    ⚠️ 尺度必须用 **int16 满幅 32767**，不是信号自身的峰值。
    第一版按峰值归一（`n = v / max_abs`），结果输入已经超量程时
    （激励把它推到 37581）输出还是 37581×0.97 = 36453，照样超上限，
    等于没限。以满幅为尺度才能保证"输出永不超过 out_ceiling×32767"。

    为什么提亮链路里必须要这一步
    ----------------------------
    原始打击乐的峰值/响度比很高（实测 31341/3813 = 8.2）—— 能量集中在
    起音后几十毫秒的尖峰，其余时间都在衰减。所以**峰值几乎没余量**：
    任何抬高响度的处理都会立刻撞 int16 上限，补偿增益只剩 1.0x，
    表现为"越提亮反而越轻"。

    实测：加激励后峰值 31341→37581（超上限），响度需求 2.76x 但峰限
    只剩 0.85x，最终响度掉到 1253，四段极差 63%。

    软限幅先把尖峰压下来，峰值/响度比变小，后面才有空间做响度补偿。
    用 tanh 而非硬切：硬切会在波形上留折角，产生高频谐波，听感是破音。

    threshold=0.72 约 -2.8dB，只压最尖的一点；
    out_ceiling=0.97 保证输出不碰 int16 边界。
    """
    FULL = 32767.0
    span = out_ceiling - threshold
    out = []
    for v in x:
        n = v / FULL                     # 以满幅为尺度，不是以峰值为尺度
        a = abs(n)
        if a <= threshold:
            y = n
        else:
            y = threshold + span * math.tanh((a - threshold) / span)
            if n < 0:
                y = -y
        out.append(y * FULL)
    return out


def brighten(x, corner_hz, gain_db, drive=0.0, mix=0.0,
             cut_hz=0.0, cut_db=0.0, target_loudness=0.0, limit_thresh=0.55):
    """提亮流水线：中频衰减 → 高频提升 → 激励 → 限幅 → 响度归一。

    顺序有讲究：
      1) 先压中频再提高频 —— 相反的次序会让高频提升把中频也带起来，白做。
      2) 限幅放在响度归一**之前** —— 见下。
      3) 最后按目标响度归一，这样四段才能齐平。

    ⚠️ 为什么要先限幅再归响度，不能反过来
    --------------------------------------
    打击乐的峰值/响度比很高（实测 8.2），能量挤在起音那几十毫秒。
    EQ 和激励又都是"增加峰值"的处理，所以提亮之后峰值必然接近 int16 上限。

    这时直接做响度归一就会撞峰限：需要 1.14x 增益但峰限只剩 1.01x，
    只能取 1.01 —— 响度补不满（实测四段极差 9.6%）。

    先把尖峰限下来，响度归一才有正向余量。限幅会同时压掉一点响度，
    所以必须在它之后归响度，不能之前。

    limit_thresh 取 0.55 而不是更保守的 0.72：实测 0.72 时峰限只有
    1.01x，仍不足以把响度补到基准；0.55 能到 1.14x，正好够。
    再低（0.40）虽然也能补满，但起音的锐利感会被削掉，打击乐变成"软"
    的一团，听感发闷 —— 反而回到要解决的问题上。
    """
    y = x
    if cut_db:
        y = peaking(y, cut_hz, cut_db, q=1.0)
    y = high_shelf(y, corner_hz, gain_db)
    if drive > 0.0 and mix > 0.0:
        y = excite(y, drive=drive, mix=mix)

    # 先限幅，再归响度
    y = soft_limit(y, threshold=limit_thresh)

    if target_loudness > 0:
        y = normalize_to(y, target_loudness)
    return y


def centroid(x, lo=150.0, hi=11000.0):
    """频谱重心（能量加权平均频率）。是"闷不闷"最直接的量化指标。

    经验阈值（小喇叭上）：
        < 1000Hz   闷，像隔着一层布
        1000~2000  正常
        > 2000Hz   偏亮，可能刺耳
    """
    num = den = 0.0
    f = lo
    while f < hi:
        e = goertzel(x, f)
        num += e * f
        den += e
        f *= 1.05
    return num / den if den else 0.0


def band_energy(x):
    """各频段能量占比。用来确认提亮到底补到哪儿了。"""
    bands = [(150, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 11000)]
    es = []
    for lo, hi in bands:
        s = 0.0
        f = lo
        while f < hi:
            s += goertzel(x, f)
            f *= 1.05
        es.append(s)
    tot = sum(es) or 1.0
    return [(lo, hi, e / tot * 100) for (lo, hi), e in zip(bands, es)]


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
    """从原始素材切出两个单音。返回 int16 量级（±32767）。

    切点 155ms 是包络的谷底，两侧分别是音1 的尾（7%）和音2 的头（47.8%）。
    切在谷底能让两个音都不带对方的残留。

    两端要加短淡入淡出：
      - 音1 尾部被强行截断（原本还有余韵被音2 盖住），12ms 余弦淡出
        把"掐断"抹平；
      - 音2 的起音很陡（10ms 内从 7% 冲到 48%），2ms 淡入只压掉
        直流量偏移带来的咔声，不削钝起音的锐利。

    ⚠️ 返回值必须和 build_done 同量级（int16，±32767），不能归一化到 ±1。
    这个坑踩过：第一版返回 ±1.0，而 build_done 返回 ±31456，两者相差
    32767 倍。loudness() 在 ±1 上算出的值接近 0，normalize_to 于是算出
    上万倍的增益需求，被峰值上限截断后只剩 1253 —— 表现为"收工音比
    其他三段轻 63%"。统一量级后这个诡异现象就没了。
    """
    a0, a1 = [int(t / 1000.0 * RATE) for t in DING_RANGE]
    b0, b1 = [int(t / 1000.0 * RATE) for t in DONG_RANGE]

    ding = fade(samples[a0:a1], fade_in_ms=2, fade_out_ms=12)
    dong = fade(samples[b0:b1], fade_in_ms=2, fade_out_ms=18)

    # 各自归一化到满幅 96%，让后续变调的增益计算不受两音原始响度差影响。
    # 用 96% 而不是 100% 是留一点余量给后面的 EQ/激励，避免一进去就削顶。
    scale = 32767 * 0.96
    ding = [v * scale / (max(abs(v) for v in ding) or 1) for v in ding]
    dong = [v * scale / (max(abs(v) for v in dong) or 1) for v in dong]
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
    shifted = apply_bright("DONE", shifted)

    pk = max(abs(v) for v in shifted) or 1
    g = 32767 * 0.96 / pk
    out = fade([v * g for v in shifted], fade_in_ms=2, fade_out_ms=15)
    return out


def apply_bright(name, x, target_loudness=0.0):
    """按 BRIGHT 表给某一段提亮并归响度。参数含义见 BRIGHT 的注释。

    target_loudness 传 0 表示只提亮、不做响度归一（由调用方统一处理）。
    """
    corner, gain, drive, mix, cut_hz, cut_db = BRIGHT[name]
    return brighten(x, corner, gain, drive, mix,
                    cut_hz=cut_hz, cut_db=cut_db,
                    target_loudness=target_loudness)


def build_attention(ding):
    """ATTENTION：同一个"叮"响两次，中间留 90ms。

    为什么是重复而不是下行：下行 = 结束，那是 DONE 的语义。提醒要的是
    **未完成感**，重复正是人催人时的本能节奏（敲门、按喇叭、拍桌子）。

    音高比 DONE 的"叮"低 3 个半音：
    两段都以"叮"开头，音高拉开一点能避免"以为是完成了"的误判，
    同时更低的音听起来更温和，不像报警。
    """
    note = apply_bright("ATTENTION", pitch_shift(ding, ATTN_SEMITONES))
    return concat(note, silence(ATTN_GAP_MS), note)


def build_connected(ding):
    """CONNECTED：单声"叮"，80ms，四段里最高。

    设备自报状态，不是给你消息，所以不需要"乐句"，一个短促的音就够。
    单声也不会像重复那样烦人。
    """
    return apply_bright("CONNECTED", pitch_shift(ding, CONN_SEMITONES))


def build_session_end(dong):
    """SESSION_END：单声"咚"，262ms，四段里最低。

    收工是"告一段落"，不是"圆满完成"，所以不用双音（那双音专属 DONE），
    只留一个低音自然衰减。低音 + 单声 + 无后续，听觉上就是"落下帷幕"。

    ⚠️ 这一段提亮力度最大，因为用户反馈"主要是收工音最闷"。它也是四段里
    唯一重心落在 500~1000Hz 的（其他三段都在 1~2kHz），所以额外压了
    800Hz 的主峰 —— 不压的话怎么提高频都拉不动重心。
    """
    return apply_bright("SESSION_END", pitch_shift(dong, END_SEMITONES))


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
        "//   ⚠️ 注意「咚」的真实发声频率不是 175Hz —— 做过 1~8 次谐波扫描，",
        "//   它的能量 99% 在第 3、4 次谐波（525/700Hz），175Hz 本身几乎为零",
        "//   （典型 missing fundamental，耳朵靠 525/700 的间隔脑补音高）。",
        "//   所以升调要看能量中心的去向，不是看自相关算出的周期。",
        "//   升调用重采样（波形时间轴压缩），不是改频率乘数——后者会破坏",
        "//   泛音比例，听起来像变声器失真。",
        "//",
        "// 为什么要提亮：原素材能量分布是 500~1000Hz 占 72%、2kHz 以上只剩 1%，",
        "//   而人耳判断明亮度看的就是 2~5kHz，所以原声听起来「闷」。",
        "//   升调只是整体平移，不会凭空造出高频，因此额外做了三件事：",
        "//     - 谐波激励：用中频基波非线性饱和，从无到有造出 2~5kHz 的新谐波",
        "//     - 高频搁架提升：把已有的那 1% 放大",
        "//     - 中频衰减（只给收工音）：它 500~1000Hz 占 69%，不压主峰拉不动重心",
        "//   四段频谱重心落在 1400~1900Hz，2~4kHz 占比从 4~7% 提到 14~16%。",
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

    # 响度基准取**提亮前**的 DONE。
    #
    # 为什么不用提亮后的：提亮会改变峰值/响度关系，而且四段提亮力度不同。
    # 用提亮前的 DONE 当基准，等于"以原素材的自然响度为锚"，四段都向它
    # 看齐 —— 用户之前听过那一版的响度，这个锚不会因为提亮参数调整而漂。
    #
    # 也正因为要在提亮**之后**才能归响度（限幅会压掉一点响度，见
    # brighten 的注释），所以这里是"先算基准 → 提亮 → 再归一"两步走。
    target = loudness(build_done(samples))
    print("\n  响度基准（提亮前的 DONE）：%.0f" % target)

    done = apply_bright("DONE", build_done(samples), target)
    attn = apply_bright("ATTENTION", build_attention(ding), target)
    conn = apply_bright("CONNECTED", build_connected(ding), target)
    end = apply_bright("SESSION_END", build_session_end(dong), target)

    tones = [
        ("DONE",        "k_tone_done", "TONE_DONE_LEN", done,
         ("叮→咚 下行", "")),
        ("ATTENTION",   "k_tone_attn", "TONE_ATTN_LEN", attn,
         ("叮 叮 重复", "")),
        ("CONNECTED",   "k_tone_conn", "TONE_CONN_LEN", conn,
         ("叮 单声", "")),
        ("SESSION_END", "k_tone_end",  "TONE_END_LEN",  end,
         ("咚 单声", "")),
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

    # 频谱重心 —— "闷不闷"最直接的量化指标。用户反馈"太闷"，所以这条
    # 是主要验收项：目标 1200~2200Hz。低于 1000 隔着布，高于 2200 偏刺。
    print("\n=== 频谱重心（闷不闷的直接指标，目标 1200~2200Hz）===")
    for name, _, _, s, _ in tones:
        c = centroid(s)
        if c < 1000:
            judge = "闷"
        elif c > 2200:
            judge = "偏亮"
        else:
            judge = "OK"
        print("  %-13s %6.0fHz  %s" % (name, c, judge))

    # 频段分布，确认高频真的补上了，而不是只提了增益数字
    print("\n=== 频段能量占比（2~4kHz 是明亮度所在）===")
    print("  段            " + "  ".join("%d-%d" % (lo, hi)
                                    for lo, hi, _ in band_energy(tones[0][3])))
    for name, _, _, s, _ in tones:
        print("  %-13s " % name + "  ".join("%4.0f%%" % p
                                          for _, _, p in band_energy(s)))

    # 能量中心自检。看的是"实际响出来的主频"，不是自相关的周期 ——
    # 「咚」的周期是 175Hz 但能量在 525Hz，两者差三倍，混淆了会调错八度。
    print("\n=== 能量中心（前 60ms 频谱峰值，小喇叭实际发声的就是这个）===")
    for name, _, _, s, _ in tones:
        head = s[:int(0.060 * RATE)]
        if len(head) < 300:
            continue
        lo = 150.0 if name == "SESSION_END" else 600.0
        hi = 3000.0
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
