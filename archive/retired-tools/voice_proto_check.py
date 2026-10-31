#!/usr/bin/env python3
"""语音上行组包交叉验证：固件 wb_proto_enc_voice() ↔ 主机真实收流代码。

为什么需要这个脚本
------------------
语音上行最容易出错的地方**不是编码**（那部分由 adpcm_crosscheck.py 逐字节
锁死），而是 **JSON 契约**：

  * 主机 _dispatch_obj() 先判 obj["voice"] == 1 才进语音分支。固件早期版本
    发的是 {"type":"voice",...} —— 没有 voice 键，于是掉进 type 分发里
    无人处理，整包**静默丢弃**。设备端看不出任何异常，只是主机永远收不到声音。
  * recording 的下降沿是主机唯一认可的"录完了"信号，漏带就永远不落盘。
  * sequence 是"本批首帧序号"，主机拿它 + 本批帧数对下一批；算法与固件
    的递增方式必须一致。
  * 上行到底受不受 480B 限制：BLE_FRAME_MAX 只约束下行写特征值，上行走
    notify + 按 '\n' 拼行。这个结论如果搞反，就会白白把每包压到 1 帧。

所以这里做端到端：C 编码 → 组包 → 用**主机真实代码**（VoiceSession.add +
_adpcm_decode，从 wb_app.py 里 ast 抽出来）还原 PCM → 与原信号比 SNR。

用法
----
    python3 tools/voice_proto_check.py

退出码 0 = 通过；1 = 有断言失败。
"""
import ast
import base64
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import textwrap

# 定位项目根：脚本住在 tools/ 的子目录里，逐级数层数会随整理失效，
# 统一走 tools/_root.py 的"向上查骨架目录"（见该文件说明）。
_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)
from _root import project_root
ROOT = str(project_root())
FIRMWARE = os.path.join(ROOT, 'firmware', 'main')
HOST_APP = os.path.join(ROOT, 'host', 'wb_app.py')

SAMPLES_PER_FRAME = 320
BYTES_PER_FRAME = 163
RATE = 16000


def _read_frames_per_pkt() -> int:
    """从固件头文件读每包帧数。

    这个值改过（2 → 1，起因是 macOS CoreBluetooth 把 MTU 封在 256，
    2 帧包要切 3 片 notify 把 mbuf 抽干）。脚本里写死会在下次调整时
    静默失效 —— 断言全绿但量的根本不是实发配置。
    """
    hdr = os.path.join(FIRMWARE, 'wb_proto.h')
    with open(hdr, encoding='utf-8') as f:
        for ln in f:
            if ln.strip().startswith('#define WB_VOICE_FRAMES_PER_PKT'):
                return int(ln.split()[-1])
    raise SystemExit(f'没能从 {hdr} 读到 WB_VOICE_FRAMES_PER_PKT')


FRAMES_PER_PKT = _read_frames_per_pkt()

# 一包全字段的实测上限，按每包帧数折算：1 帧 363B，2 帧 579B。
# 留 ~40B 余量做断言上限，超出说明骨架又长胖了。
LINE_MAX_EXPECT = 400 + 220 * (FRAMES_PER_PKT - 1)

CLI_SRC = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wb_adpcm.h"
#include "wb_proto.h"

static void flush_batch(uint8_t *batch, int n, int session, int recording,
                        int seq, int peak, int dropped, int clipped,
                        int samples, int stop_reason)
{
    wb_voice_pkt_t p;
    char line[WB_VOICE_PKT_MAX];
    memset(&p, 0, sizeof(p));
    p.adpcm = batch;
    p.n_frames = (size_t)n;
    p.session = session;
    p.recording = recording != 0;
    p.sequence = seq;
    p.peak = peak;
    p.dropped = dropped;
    p.clipped = clipped;
    p.samples = samples;
    p.stop_reason = stop_reason;

    size_t len = wb_proto_enc_voice(line, sizeof(line), &p);
    if (len == 0) { fprintf(stderr, "enc_voice 失败（缓冲区不够？）\n"); exit(3); }
    fwrite(line, 1, len, stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s in.raw [session]\n", argv[0]); return 2; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("in"); return 1; }
    const int session = (argc > 2) ? atoi(argv[2]) : 1;

    wb_adpcm_state_t st;
    wb_adpcm_init(&st);
    int16_t samples[WB_ADPCM_SAMPLES];
    uint8_t batch[WB_ADPCM_FRAME_BYTES * WB_VOICE_FRAMES_PER_PKT];

    int seq = 0, peak = 0, total = 0, clipped = 0, n = 0;

    for (;;) {
        size_t got = fread(samples, sizeof(int16_t), WB_ADPCM_SAMPLES, fi);
        if (got != WB_ADPCM_SAMPLES) break;
        for (int i = 0; i < WB_ADPCM_SAMPLES; i++) {
            int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) peak = v;
            if (v >= 32767) clipped++;
        }
        wb_adpcm_encode_frame(&st, samples, batch + n * WB_ADPCM_FRAME_BYTES);
        n++;
        total += WB_ADPCM_SAMPLES;
        if (n == WB_VOICE_FRAMES_PER_PKT) {
            flush_batch(batch, n, session, 1, seq, peak, 0, clipped, total, 0);
            seq += n;
            n = 0;
        }
    }
    /* 尾巴：recording=false + stop_reason=1（正常松开），主机靠这个下降沿收尾。
     * n 可能是 0（输入正好是整包）—— 这时仍然必须发一个 0 帧包，
     * 它是唯一的收尾信号（主机 adpcm 为空 → count=0）。 */
    flush_batch(batch, n, session, 0, seq, peak, 0, clipped, total, 1);
    seq += n;
    fclose(fi);
    fprintf(stderr, "frames=%d seq_end=%d peak=%d clipped=%d samples=%d tail=%d\n",
            seq, seq, peak, clipped, total, n);
    return 0;
}
'''


def load_host_rx():
    """从 host/wb_app.py 抽出真实的 _adpcm_decode / _as_int / VoiceSession。

    测的必须是同一份代码：如果主机改了收流逻辑而固件没跟，这里立刻红。
    """
    src = open(HOST_APP, encoding='utf-8').read()
    tree = ast.parse(src)

    want = {'_adpcm_decode', '_as_int', 'VoiceSession'}
    segs = {}
    for node in ast.walk(tree):
        name = getattr(node, 'name', None)
        if name in want and name not in segs:
            segs[name] = ast.get_source_segment(src, node)
    missing = want - set(segs)
    if missing:
        raise RuntimeError(f'host/wb_app.py 里找不到 {sorted(missing)}')

    # 常量照抄（这几个是模块级 tuple，ast 抽 class 时带不过来）
    ns = {
        '__builtins__': __builtins__,
        'base64': base64,
        'time': __import__('time'),
        'VOICE_FRAME_BYTES': BYTES_PER_FRAME,
        'VOICE_SAMPLE_RATE': RATE,
        '_ADPCM_STEP_TABLE': _STEP,
        '_ADPCM_INDEX_TABLE': _IDX,
    }
    for name in ('_adpcm_decode', '_as_int'):
        exec(textwrap.dedent(segs[name]), ns)
    exec(textwrap.dedent(segs['VoiceSession']), ns)
    return ns


_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767,
)
_IDX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def build_signal(frames=31):
    """31 帧（620ms），故意**不是**每包帧数的整数倍 —— 尾巴包才能走到
    recording=false 那条路径，否则测不到主机收尾。"""
    out = []
    for k in range(frames):
        for i in range(SAMPLES_PER_FRAME):
            if k < 2:
                v = 0
            elif k < 12:
                v = int(9000 * math.sin(2 * math.pi * 440 * (k * SAMPLES_PER_FRAME + i) / RATE))
            elif k < 24:
                f = 300 + 900 * i / SAMPLES_PER_FRAME
                v = int(8000 * math.sin(2 * math.pi * f * (k * SAMPLES_PER_FRAME + i) / RATE))
            else:
                v = int(6000 * math.sin(2 * math.pi * 190 * (k * SAMPLES_PER_FRAME + i) / RATE))
            out.append(max(-32768, min(32767, v)))
    return out


def snr_db(ref, got):
    """按参考信号功率比误差功率。长度不等直接判 0。"""
    n = min(len(ref), len(got))
    if n == 0:
        return 0.0
    sig = sum(float(ref[i]) ** 2 for i in range(n))
    err = sum((float(ref[i]) - float(got[i])) ** 2 for i in range(n))
    if err <= 0:
        return 99.0
    if sig <= 0:
        return 0.0
    return 10 * math.log10(sig / err)


def build_cli(tmpdir):
    exe = os.path.join(tmpdir, 'voice_cli')
    sources = ['wb_adpcm.c', 'wb_proto.c', 'wb_json.c']
    cmd = ['cc', '-O1', '-std=c99', '-I', FIRMWARE, '-o', exe,
           os.path.join(tmpdir, 'cli.c')] + [os.path.join(FIRMWARE, s) for s in sources]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        raise RuntimeError('CLI 编译失败')
    return exe


def main():
    fails = []

    def check(ok, msg):
        print(('  [ok]   ' if ok else '  [FAIL] ') + msg)
        if not ok:
            fails.append(msg)

    samples = build_signal()
    raw = b''.join(struct.pack('<h', s) for s in samples)

    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, 'cli.c')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(CLI_SRC)
        exe = build_cli(td)
        inp = os.path.join(td, 'in.raw')
        with open(inp, 'wb') as f:
            f.write(raw)

        r = subprocess.run([exe, inp, '7'], capture_output=True)
        if r.returncode != 0:
            print(r.stderr.decode('utf-8', 'replace'))
            raise RuntimeError('CLI 运行失败')
        cli_note = r.stderr.decode('utf-8', 'replace').strip()

    out = r.stdout
    print(f'固件产出 {len(out)}B；CLI: {cli_note}')

    lines = out.split(b'\n')
    check(lines and lines[-1] == b'', '整批以 \\n 收尾（主机靠它切行）')
    lines = [ln for ln in lines if ln]

    print(f'\n== 1. 行格式与体积（{len(lines)} 行，{FRAMES_PER_PKT} 帧/包）==')
    n_in = len(samples) // SAMPLES_PER_FRAME
    # n // F 个满包，外加**恒定存在**的收尾包（帧数可能为 0 —— 正好整除时
    # 收尾包没有音频，但 recording=false 是主机唯一认的收尾信号，必须发）。
    expect_lines = n_in // FRAMES_PER_PKT + 1
    check(len(lines) == expect_lines,
          f'行数 = {n_in} // {FRAMES_PER_PKT} + 1 个收尾包 = {len(lines)}'
          f'（应为 {expect_lines}）')
    check(all(len(ln) <= LINE_MAX_EXPECT for ln in lines),
          f'每行 ≤ {LINE_MAX_EXPECT}B（最宽 {max(len(l) for l in lines)}B）')

    objs = []
    ok_json = True
    for ln in lines:
        try:
            objs.append(json.loads(ln.decode('utf-8')))
        except Exception as exc:
            ok_json = False
            print(f'    json 失败: {exc} / {ln[:60]!r}')
    check(ok_json, '每行都是合法 JSON')

    print('\n== 2. 键契约（主机 _dispatch_obj 的分流条件）==')
    check(all(o.get('voice') == 1 for o in objs), '每行都有 voice == 1（否则主机不进语音分支）')
    check(all('type' not in o for o in objs),
          '每行都**没有** type 键（带了会掉进 type 分发里静默丢弃）')
    need = ('session', 'recording', 'sequence', 'peak', 'dropped', 'clipped',
            'samples', 'stop_reason', 'adpcm')
    check(all(all(k in o for k in need) for o in objs),
          f'九个字段齐全: {", ".join(need)}')
    check(all(isinstance(o['recording'], bool) for o in objs),
          'recording 是 JSON 布尔而不是字符串')
    check(sum(1 for o in objs if not o['recording']) == 1,
          '恰好一行 recording=false（收尾下降沿，多一行会让主机会话反复重建）')
    check(objs[-1]['recording'] is False, '最后一行是收尾包')
    check(objs[-1]['stop_reason'] == 1, '收尾包带 stop_reason=1（正常松开）')
    check(all(o['session'] == 7 for o in objs), 'session 一致')

    print('\n== 3. 帧数与序号 ==')
    frame_counts = []
    for o in objs:
        blobs = base64.b64decode(o['adpcm'])
        check3 = len(blobs) % BYTES_PER_FRAME == 0
        if not check3:
            fails.append(f'adpcm 长度 {len(blobs)} 不是 {BYTES_PER_FRAME} 的整数倍')
        frame_counts.append(len(blobs) // BYTES_PER_FRAME)
    # 中间包必须满 FRAMES_PER_PKT 帧，只允许最后一包是尾巴（可以是 0 帧）。
    body = frame_counts[:-1]
    check(all(c == FRAMES_PER_PKT for c in body),
          f'中间每包 {FRAMES_PER_PKT} 帧: {frame_counts}')
    check(0 <= frame_counts[-1] <= FRAMES_PER_PKT,
          f'末包 0..{FRAMES_PER_PKT} 帧: {frame_counts[-1]}')
    check(sum(frame_counts) == len(samples) // SAMPLES_PER_FRAME,
          f'总帧数 {sum(frame_counts)} == 输入帧数 {len(samples) // SAMPLES_PER_FRAME}')

    # sequence 必须是"本批首帧序号"：前一行 sequence + 前一行帧数 == 本行 sequence
    seq_ok = True
    expect = objs[0]['sequence']
    for o, c in zip(objs, frame_counts):
        if o['sequence'] != expect:
            seq_ok = False
            print(f"    sequence 断点: 期望 {expect} 实际 {o['sequence']}")
        expect = o['sequence'] + c
    check(seq_ok, 'sequence 逐包连续（主机靠它算丢批）')

    print('\n== 4. 用主机真实代码还原 PCM ==')
    host = load_host_rx()
    sess = host['VoiceSession'](7, 'reply')
    for o in objs:
        sess.add(o)
    got = list(struct.unpack(f'<{len(sess.pcm) // 2}h', sess.pcm))
    check(sess.frames == len(samples) // SAMPLES_PER_FRAME, f"主机收到 {sess.frames} 帧")
    check(sess.lost == 0, f'主机判定丢帧 {sess.lost}（应为 0）')
    check(sess.stop_reason == 1, f'主机读到 stop_reason={sess.stop_reason}')
    check(abs(sess.seconds * RATE - len(samples)) < RATE * 0.05,
          f'时长 {sess.seconds:.3f}s ≈ {len(samples) / RATE:.3f}s')

    q = snr_db(samples, got)
    check(q >= 20.0, f'还原波形 SNR {q:.1f}dB（阈值 20dB，ADPCM 理论约 26dB）')

    print('\n== 5. 体积与主机上行的真实上限 ==')
    widest = max(len(l) for l in lines)
    # 两种 MTU 都要看：Windows 下能协商到 512（1 片），macOS CoreBluetooth
    # 固定封在 256（payload 253B）。后者是实际踩坑的那条路径 —— 切片数多一片，
    # NimBLE 的 12 块 msys 池就更容易被抽干，主机侧表现为"轮询写超时"断链。
    for mtu in (512, 256):
        payload = mtu - 3
        print(f'   最宽一包 {widest}B → MTU {mtu}（{payload}B/片）切 '
              f'{-(-widest // payload)} 片')
    check(widest < 64 * 1024, '远小于主机 MAX_LINE_BYTES=64KB')
    check(-(-widest // 253) <= 2, f'最低 MTU 256 下也只切 2 片（实际 {-(-widest // 253)} 片）')
    print('   注：主机 BLE_FRAME_MAX=480 只约束**下行**写特征值；上行走 notify，'
          '按 \\n 拼行，不受 480 限制。')

    print('\n== 6. 0 帧收尾包（录音正好停在整包边界）==')
    # 固件里这是很常见的一条路径：2 帧/包，只要用户按住时长恰好是 40ms 的
    # 整数倍，尾巴就是 0 帧。这一包没有音频，但 recording=false 是主机
    # **唯一**认的"录完了"，丢了它主机会一直挂着会话不落 WAV。
    samples2 = build_signal(frames=30)      # 30 帧 = 15 个整包
    raw2 = b''.join(struct.pack('<h', s) for s in samples2)
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, 'cli.c'), 'w', encoding='utf-8') as f:
            f.write(CLI_SRC)
        exe = build_cli(td)
        inp = os.path.join(td, 'in.raw')
        with open(inp, 'wb') as f:
            f.write(raw2)
        r2 = subprocess.run([exe, inp, '9'], capture_output=True)
        if r2.returncode != 0:
            print(r2.stderr.decode('utf-8', 'replace'))
            raise RuntimeError('0 帧尾巴用例：CLI 运行失败')
    lines2 = [l for l in r2.stdout.split(b'\n') if l]
    objs2 = [json.loads(l.decode('utf-8')) for l in lines2]
    check(objs2[-1]['recording'] is False, '最后一包 recording=false')
    check(objs2[-1]['adpcm'] == '', '最后一包 adpcm 为空串（0 帧）')
    check(objs2[-1]['stop_reason'] == 1, '最后一包带 stop_reason=1')
    host2 = load_host_rx()
    sess2 = host2['VoiceSession'](9, 'reply')
    for o in objs2:
        sess2.add(o)
    check(sess2.frames == 30, f'主机仍收到 30 帧（实际 {sess2.frames}）')
    check(sess2.lost == 0, f'主机判定丢帧 {sess2.lost}（0 帧包不该被算成断口）')

    print()
    if fails:
        print(f'✗ {len(fails)} 项失败')
        for m in fails:
            print('  - ' + m)
        return 1
    print('✓ 全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
