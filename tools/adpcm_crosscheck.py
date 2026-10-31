#!/usr/bin/env python3
"""ADPCM 编码器交叉验证：C 固件 ↔ Python 参考 ↔ 主机解码器。

为什么需要这个脚本
------------------
ADPCM 是"有状态"的编码：每帧的输出依赖上一帧结束时的 predictor/index。
码表抄错一位、右移方向搞反、nibble 高低半字节顺序写错 —— 这些错误
**在设备端完全看不出来**：编码器不会报错、不会崩、帧长也对，只是主机
解出来的声音变成噪声。真机联调时才发现的代价太高，所以在 PC 上先锁死。

三方比对：
  1. 用同一份 int16 样本，分别由 firmware/main/wb_adpcm.c（编译成 CLI）
     和本脚本里的 Python 参考实现编码，要求**逐字节相同**。
  2. 把 C 的输出喂给 host/wb_app.py 里真实的 _adpcm_decode()，
     要求还原出来的波形与原信号 SNR 合理。

用法
----
    python3 tools/adpcm_crosscheck.py

跑不起来时检查：gcc 可用、host/wb_app.py 存在、firmware/main/wb_adpcm.{c,h} 存在。
退出码 0 = 通过；1 = 不一致。
"""
import ast
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, 'firmware', 'main')
HOST_APP = os.path.join(ROOT, 'host', 'wb_app.py')

SAMPLES_PER_FRAME = 320
BYTES_PER_FRAME = 163

STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
        45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
        230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
        963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
        10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
        29794, 32767)
IDX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)

CLI_SRC = r'''
#include <stdio.h>
#include <stdlib.h>
#include "wb_adpcm.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.raw out.frames\n", argv[0]); return 2; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("in"); return 1; }
    FILE *fo = fopen(argv[2], "wb");
    if (!fo) { perror("out"); fclose(fi); return 1; }

    wb_adpcm_state_t st;
    wb_adpcm_init(&st);
    int16_t samples[WB_ADPCM_SAMPLES];
    uint8_t frame[WB_ADPCM_FRAME_BYTES];
    int nf = 0;
    while (fread(samples, sizeof(int16_t), WB_ADPCM_SAMPLES, fi) == WB_ADPCM_SAMPLES) {
        wb_adpcm_encode_frame(&st, samples, frame);
        fwrite(frame, 1, WB_ADPCM_FRAME_BYTES, fo);
        nf++;
    }
    fclose(fi); fclose(fo);
    fprintf(stderr, "encoded %d frames\n", nf);
    return 0;
}
'''


def load_host_decoder():
    """从 host/wb_app.py 里抽出真实的 _adpcm_decode()，保证测的是同一份代码。"""
    src = open(HOST_APP, encoding='utf-8').read()
    tree = ast.parse(src)
    seg = None
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == '_adpcm_decode':
            seg = ast.get_source_segment(src, node)
            break
    if not seg:
        raise RuntimeError('host/wb_app.py 里找不到 _adpcm_decode()')
    ns = {'_ADPCM_STEP_TABLE': STEP, '_ADPCM_INDEX_TABLE': IDX}
    exec(textwrap.dedent(seg), ns)
    return ns['_adpcm_decode']


def encode_frame(samples, predictor, index):
    """Python 参考编码器，与 wb_adpcm.c 逻辑逐行对应。"""
    out = bytearray()
    out += struct.pack('<h', max(-32768, min(32767, predictor)))
    out += bytes([index & 0xFF])

    nib = []
    for s in samples:
        step = STEP[index]
        diff = s - predictor
        code = 8 if diff < 0 else 0
        if diff < 0:
            diff = -diff
        delta = 0
        if diff >= step:
            code |= 4
            delta += step
            diff -= step
        if diff >= (step >> 1):
            code |= 2
            delta += step >> 1
            diff -= step >> 1
        if diff >= (step >> 2):
            code |= 1
            delta += step >> 2
        delta += step >> 3

        predictor += -delta if (code & 8) else delta
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + IDX[code]))
        nib.append(code)

    for i in range(0, len(nib), 2):
        out.append((nib[i] & 0x0F) | ((nib[i + 1] & 0x0F) << 4))
    return bytes(out), predictor, index


def build_signal(frames=30):
    """混合信号：静音 / 正弦 / 类人声扫频 / 宽带噪声 / 满幅方波。

    方波是故意的极端项 —— ADPCM 追不上 ±32767 的瞬时跳变，
    它存在的意义是确认编码器在削顶时不会越界或崩溃，而不是测音质。
    """
    random.seed(7)
    out = []
    for k in range(frames):
        for i in range(SAMPLES_PER_FRAME):
            t = 2 * math.pi * (300 + 900 * i / SAMPLES_PER_FRAME) * i / 16000
            if k < 3:
                v = 0
            elif k < 8:
                v = int(12000 * math.sin(2 * math.pi * 1000 * i / 16000))
            elif k < 20:
                v = int(9000 * math.sin(t) * (0.5 + 0.5 * math.sin(math.pi * i / SAMPLES_PER_FRAME)))
            elif k < 25:
                v = random.randint(-8000, 8000)
            else:
                v = 32767 if (i // 8) % 2 else -32768
            out.append(max(-32768, min(32767, v)))
    return out


def snr_db(orig, got):
    sig = sum(x * x for x in orig)
    err = sum((a - b) ** 2 for a, b in zip(orig, got))
    return 999.0 if err == 0 else 10 * math.log10(sig / err)


def main():
    for p in (HOST_APP, os.path.join(FIRMWARE, 'wb_adpcm.c'), os.path.join(FIRMWARE, 'wb_adpcm.h')):
        if not os.path.exists(p):
            print(f'缺少文件: {p}', file=sys.stderr)
            return 1

    decode = load_host_decoder()
    tmp = tempfile.mkdtemp(prefix='adpcm_xc_')
    cli_src = os.path.join(tmp, 'cli.c')
    cli_bin = os.path.join(tmp, 'cli')
    raw_path = os.path.join(tmp, 'in.raw')
    frame_path = os.path.join(tmp, 'out.frames')

    open(cli_src, 'w').write(CLI_SRC)
    cc = subprocess.run(['gcc', '-std=c99', '-O2', '-Wall', '-Wextra',
                         '-I', FIRMWARE, cli_src, os.path.join(FIRMWARE, 'wb_adpcm.c'),
                         '-o', cli_bin], capture_output=True, text=True)
    if cc.returncode != 0:
        print('C 编译失败:\n' + cc.stderr, file=sys.stderr)
        return 1

    samples = build_signal()
    open(raw_path, 'wb').write(struct.pack('<%dh' % len(samples), *samples))

    run = subprocess.run([cli_bin, raw_path, frame_path], capture_output=True, text=True)
    if run.returncode != 0:
        print('C 编码执行失败:\n' + run.stderr, file=sys.stderr)
        return 1
    c_frames = open(frame_path, 'rb').read()

    py_frames = bytearray()
    pred, idx = 0, 0
    for f in range(len(samples) // SAMPLES_PER_FRAME):
        chunk = samples[f * SAMPLES_PER_FRAME:(f + 1) * SAMPLES_PER_FRAME]
        fb, pred, idx = encode_frame(chunk, pred, idx)
        py_frames += fb

    ok = True
    n_c = len(c_frames) // BYTES_PER_FRAME
    print(f'帧数: C={n_c}  Py={len(py_frames) // BYTES_PER_FRAME}  '
          f'字节: C={len(c_frames)}  Py={len(py_frames)}')
    if c_frames != bytes(py_frames):
        ok = False
        print('✗ C 与 Python 编码器不一致')
        for i in range(min(len(c_frames), len(py_frames))):
            if c_frames[i] != py_frames[i]:
                print(f'  首个差异 @ 字节 {i}（帧 {i // BYTES_PER_FRAME} '
                      f'内偏移 {i % BYTES_PER_FRAME}）: C=0x{c_frames[i]:02x} '
                      f'Py=0x{py_frames[i]:02x}')
                break
    else:
        print('✓ C 与 Python 编码器逐字节一致')

    dec = b''.join(decode(c_frames[i:i + BYTES_PER_FRAME])
                   for i in range(0, len(c_frames), BYTES_PER_FRAME))
    got = struct.unpack('<%dh' % (len(dec) // 2), dec)

    print('\n用主机 _adpcm_decode() 还原 C 的输出：')
    print(f"{'段':16s}{'SNR(dB)':>10s}")
    segs = [(0, 3, '静音'), (3, 8, '1kHz 正弦'), (8, 20, '类人声扫频'),
            (20, 25, '宽带噪声'), (25, 30, '满幅方波')]
    for a, b, name in segs:
        s0, e0 = a * SAMPLES_PER_FRAME, b * SAMPLES_PER_FRAME
        print(f'{name:16s}{snr_db(samples[s0:e0], got[s0:e0]):>10.1f}')

    # 语音段是最能代表真实使用的：低于 20dB 说明编码链路有问题
    s0, e0 = 8 * SAMPLES_PER_FRAME, 20 * SAMPLES_PER_FRAME
    voice_snr = snr_db(samples[s0:e0], got[s0:e0])
    print(f'\n语音段 SNR = {voice_snr:.1f} dB（阈值 20dB）')
    if voice_snr < 20.0:
        ok = False
        print('✗ 语音段质量不达标')

    print('\n结论:', '通过' if ok else '失败')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
