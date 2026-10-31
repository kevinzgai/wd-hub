#!/usr/bin/env python3
"""真机语音上行抓包：设备 → BLE → 解码 → WAV。

为什么不用 host/wb_app.py 测：那套是 Windows 专用（winmm 虚拟声卡 +
输入法听写），在 macOS 上跑不起来。而语音上行这段链路（采集 → ADPCM →
base64 → notify → 主机拼行 → 解码）跟平台无关，所以这里写一个最小接收器，
只为验证**设备发出的东西主机能不能正确还原**。

它做的事：
  1. 扫 Workbuddy BOX → 连上 → 订阅 notify 特征
  2. 周期发一条 status_list（设备靠"收到过下行帧"才认为已连接，
     没有它控制台页面根本出不来，第 6 屏也就进不去）
  3. 按 '\\n' 拼行，voice 行交给 wb_app.py 里**真实的** VoiceSession 解码
  4. recording 下降沿 → 落 WAV + 打印统计

用法
----
    python3 tools/voice_hw_capture.py                 # 扫设备、连、收 90 秒
    python3 tools/voice_hw_capture.py -t 180 -o ~/tmp # 收 180 秒，WAV 放指定目录

设备上怎么操作：配额页长按 → 运行详情 → 长按 → 发语音页 → 按住 OK 说话，
松手发送。短按是换投递目标。
"""
import argparse
import asyncio
import json
import os
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bleak import BleakClient, BleakScanner

# 借用 PC 侧验证脚本里"抽出主机真实收流代码"的实现 —— 这里必须是同一份代码，
# 否则测的就不是主机真正会做的那件事了。
from voice_proto_check import RATE, load_host_rx

DEV_NAME = "Workbuddy BOX"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

# 设备侧 wb_core_tick() 6 秒没收到下行帧就判离线、界面退回看板屏，
# 所以 2 秒一条足够。n_sess 至少 1 且状态 RUNNING：配额页长按要
# n_sess > 0 才进得去运行详情，再长按才到发语音页。
STATUS_FRAME = (
    b'{"type":"status_list","count":1,"tasks":[{"state":"RUNNING",'
    b'"model":"hw-capture","name":"\xe8\xaf\xad\xe9\x9f\xb3\xe9\x93\xbe\xe8\xb7\xaf\xe6\xb5\x8b\xe8\xaf\x95",'
    b'"msg":"","credit":1.0}]}'
)
STATUS_INTERVAL = 2.0


class Capture:
    def __init__(self, out_dir: str) -> None:
        self.host = load_host_rx()
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.rx = bytearray()
        self.sess = None
        self.sessions = 0
        self.t0 = time.time()

    def on_notify(self, _ch, data: bytearray) -> None:
        self.rx.extend(data)
        while True:
            cut = self.rx.find(b"\n")
            if cut < 0:
                break
            raw = bytes(self.rx[:cut])
            del self.rx[: cut + 1]
            if raw.strip():
                self.on_line(raw)

    def on_line(self, raw: bytes) -> None:
        text = raw.decode("utf-8", "replace").strip()
        if not text.startswith("{"):
            # 裸文本 ACK：OK_STATUS / OK_CFG:<vol> / ERR_JSON
            print(f"  <- ACK {text}")
            return
        try:
            obj = json.loads(text)
        except ValueError:
            print(f"  <- 坏行 {len(raw)}B: {text[:70]}…")
            return
        if not isinstance(obj, dict):
            return
        if obj.get("voice") == 1:
            self.on_voice(obj)
            return
        print(f"  <- {text[:100]}")

    def on_voice(self, obj: dict) -> None:
        sid = obj.get("session")
        rec = bool(obj.get("recording"))

        if self.sess is not None and self.sess.session_id != sid:
            print(f"  !! 上一轮没等到 recording=false 就换了 session ({self.sess.session_id}→{sid})")
            self.finish()
        if rec and self.sess is None:
            self.sess = self.host["VoiceSession"](sid, "hw-capture")
            self.sessions += 1
            print(f"  >> 开始收流 session={sid}")
        if self.sess is None:
            return

        self.sess.add(obj)
        if not rec:
            self.finish()
        # 正常包（recording=true）不逐包打印：25 包/秒会把日志刷爆，
        # 真正要看的是收尾那一行统计。

    def finish(self) -> None:
        s = self.sess
        self.sess = None
        if s is None:
            return
        if not s.pcm:
            print("  << 录音结束：没有音频（麦克风没拾到？）")
            return
        name = time.strftime("hw-voice-%Y%m%d-%H%M%S") + f"-{s.session_id}.wav"
        path = os.path.join(self.out_dir, name)
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(bytes(s.pcm))
        print(f"  << 录音结束 session={s.session_id} {s.seconds:.2f}s "
              f"帧={s.frames} 丢={s.lost} 削顶={s.clipped} 峰={s.peak} "
              f"停止={s.stop_reason}")
        print(f"     -> {path}")


async def find_device(timeout: float):
    print(f"扫描 {DEV_NAME}（{timeout:.0f}s）…")
    dev = await BleakScanner.find_device_by_name(DEV_NAME, timeout=timeout)
    if dev is None:
        # 名字对不上时列一遍，方便确认设备是不是还在广播
        found = await BleakScanner.discover(timeout=5.0)
        print("没找到。当前扫到的设备：")
        for d in found:
            print(f"  {d.address}  {d.name!r}  rssi={d.rssi}")
        return None
    return dev


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=float, default=90.0)
    ap.add_argument("-o", "--out", default=os.path.expanduser("~/.workbuddy/voice"))
    ap.add_argument("--scan-timeout", type=float, default=15.0)
    args = ap.parse_args()

    dev = await find_device(args.scan_timeout)
    if dev is None:
        return 2

    cap = Capture(args.out)
    print(f"连接 {dev.name} ({dev.address}) …")
    async with BleakClient(dev, timeout=20.0) as cli:
        await cli.start_notify(CHAR_UUID, cap.on_notify)
        print("已订阅 notify。现在去设备上操作：配额页长按 → 运行详情 → 长按 → 按住 OK 说话。")
        end = time.time() + args.seconds
        try:
            while time.time() < end:
                if not cli.is_connected:
                    print("链路断开，退出")
                    break
                try:
                    await cli.write_gatt_char(CHAR_UUID, STATUS_FRAME, response=True)
                except Exception as exc:
                    print(f"  状态帧写失败: {exc}")
                await asyncio.sleep(STATUS_INTERVAL)
        finally:
            if cap.sess is not None:
                print("超时，未等到 recording=false，已收部分仍落盘：")
                cap.finish()
            try:
                await cli.stop_notify(CHAR_UUID)
            except Exception:
                pass

    print(f"共收到 {cap.sessions} 轮录音，WAV 在 {cap.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
