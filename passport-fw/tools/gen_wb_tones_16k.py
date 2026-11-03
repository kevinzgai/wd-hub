#!/usr/bin/env python3
"""把 S3 看板固件里已组装好的 24kHz 提示音切片转换成 16kHz PCM 头文件。

素材血缘：audio/candidates/feedback-confirm-41.wav（SFXMint · Gentle Marimba，CC0）。
S3 的 firmware/main/tone_pcm.h 已把这段素材切成 A/B/C 三段并组合成五种语义；
本脚本只做 24k -> 16k 重采样（3 抽 2 + 3 抽低通），不改音高、不改时长、不重新合成。

用法：python tools/gen_wb_tones_16k.py
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "firmware" / "main" / "tone_pcm.h"
TARGET = ROOT / "main" / "wb" / "wb_tone_pcm.h"
IN_RATE = 24000
OUT_RATE = 16000

TABLES = (
    ("done", "k_tone_done", "DONE"),
    ("attn", "k_tone_attn", "ATTENTION"),
    ("conn", "k_tone_conn", "CONNECTED"),
    ("end", "k_tone_end", "SESSION_END"),
    ("key", "k_tone_key", "KEY"),
)


def read_tables():
    text = SOURCE.read_text(encoding="utf-8")
    result = {}
    for name, symbol, _ in TABLES:
        match = re.search(r"static const int16_t\s+" + symbol + r"\[[^\]]*\]\s*=\s*\{([^}]*)\};", text, re.S)
        if not match:
            raise SystemExit(f"找不到 {symbol}，检查 {SOURCE}")
        result[name] = [int(v) for v in re.findall(r"-?\d+", match.group(1))]
    return result


def resample(samples):
    # 3 抽低通，抑制 8kHz 以上折叠；系数和为 1，不改变响度。
    filtered = []
    last = len(samples) - 1
    for i, value in enumerate(samples):
        prev = samples[i - 1] if i > 0 else value
        nxt = samples[i + 1] if i < last else value
        filtered.append(0.25 * prev + 0.5 * value + 0.25 * nxt)
    count = int(len(samples) * OUT_RATE / IN_RATE)
    out = []
    step = IN_RATE / OUT_RATE
    for j in range(count):
        pos = j * step
        i = int(pos)
        frac = pos - i
        if i >= last:
            out.append(int(round(filtered[last])))
        else:
            out.append(int(round(filtered[i] * (1.0 - frac) + filtered[i + 1] * frac)))
    return out


def main():
    tables = read_tables()
    lines = [
        "// 自动生成，勿手改。重新生成：python tools/gen_wb_tones_16k.py",
        "//",
        "// 素材：audio/candidates/feedback-confirm-41.wav",
        "//       SFXMint feedback-confirm-41 · Gentle Marimba (CC0 1.0)",
        "// 上游：S3 看板固件 firmware/main/tone_pcm.h 的 24kHz 切片（只复制音频数据，",
        "//       不链接其源码）。本工程按 16kHz/单声道重新采样，时长与音高不变。",
        "//",
        "// 每段都从 0 淡入、到 0 淡出，避免 I2S 边界爆音；最长一段约 702ms。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define WB_TONE_RATE {OUT_RATE}",
        "",
    ]
    for name, _, enum_name in TABLES:
        data = resample(tables[name])
        lines.append(f"// {enum_name} {len(data) * 1000 / OUT_RATE:.0f}ms")
        lines.append(f"#define WB_TONE_{name.upper()}_LEN {len(data)}")
        lines.append(f"static const int16_t k_wb_tone_{name}[WB_TONE_{name.upper()}_LEN] = {{")
        for offset in range(0, len(data), 12):
            lines.append("    " + ", ".join(map(str, data[offset:offset + 12])) + ",")
        lines.append("};")
        lines.append("")
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    for name, _, _ in TABLES:
        print(f"{name}: {len(tables[name])} -> {len(resample(tables[name]))} samples")


if __name__ == "__main__":
    main()
