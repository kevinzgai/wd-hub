#!/usr/bin/env python3
"""串口抓屏：给设备发 p，收 --SHOT-BEGIN/H:xxxx:hex/--SHOT-END 码流，还原成 PNG。

用法：
    python3 tools/capture_screen.py /dev/cu.usbmodem21201 out.png [按键序列]

按键序列用短横线分隔，可选（不传就只抓当前屏）：
    d d d          连按三次 DOWN（翻 3 页）
    y               长按 UP
    o               短按 OK
    k               长按 DOWN
    l / b           长按 OK（main.cc 串口命令，用于验证全局返回 / 回首页）
示例：
    python3 tools/capture_screen.py /dev/cu.usbmodem21201 out.png "d,d,d,d"

注意：打开串口会拉 DTR/RTS 让 ESP32-C3 复位（这是 USB-Serial-JTAG 的既定行为），
所以脚本默认先等 5s 让设备启动完、并把开机动画放过，再发按键。
"""

import argparse
import re
import struct
import sys
import time
import zlib

SHOT_W = 240
SHOT_H = 320


def write_png(path, rows, w, h):
    """把 RGB565 行数据写成 PNG。纯标准库实现，避免依赖 Pillow。

    RGB565 -> RGB888 用「高位复制」补低位：565 的 5 位红展开到 8 位时，
    把高 3 位再填回低 3 位，比简单左移更接近真实亮度（避免整体偏暗）。
    """
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # PNG filter: None
        row = rows[y]
        for x in range(w):
            px = struct.unpack_from("<H", row, x * 2)[0]
            r5 = (px >> 11) & 0x1F
            g6 = (px >> 5) & 0x3F
            b5 = px & 0x1F
            r8 = (r5 << 3) | (r5 >> 2)
            g8 = (g6 << 2) | (g6 >> 4)
            b8 = (b5 << 3) | (b5 >> 2)
            raw += bytes((r8, g8, b8))

    def chunk(tag, data):
        return (tag + struct.pack(">I", len(data)) + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


KEY_CODES = {"u": b"u", "d": b"d", "o": b"o", "k": b"k", "y": b"y",
             "l": b"l", "b": b"b"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("keys", nargs="?", default="",
                    help="按键序列，如 'd,d,d'；空则直接抓当前屏")
    ap.add_argument("--boot-wait", type=float, default=12.0,
                    help="打开串口后等多久再操作（秒）。打开串口会让 C3 复位，"
                         "要等开机动画+BLE 初始化走完、WB 应用起来后才收按键")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("需要 pyserial：pip install pyserial", file=sys.stderr)
        return 2

    ser = serial.Serial(args.port, 115200, timeout=0.2, write_timeout=2)
    try:
        time.sleep(args.boot_wait)
        ser.reset_input_buffer()

        # 复位后第一按只会「点亮屏幕」并被底座吞掉（app_registry 的唤醒护栏）。
        # 先空发一次，把这一按消耗掉，后面真正的翻页才不会被吃掉。
        ser.write(b"o")
        ser.flush()
        time.sleep(0.6)

        # 先按键，再抓屏；每次按键间隔 700ms，让 UI 有时间重建新页
        # （本工程按需建屏，切页要先删旧屏再建新屏，太快会抓到过渡态）。
        for k in [s.strip() for s in args.keys.split(",") if s.strip()]:
            code = KEY_CODES.get(k)
            if not code:
                print(f"忽略未知按键 {k!r}", file=sys.stderr)
                continue
            ser.write(code)
            ser.flush()
            time.sleep(0.7)

        ser.write(b"p")
        ser.flush()

        w = h = 0
        lines = {}
        deadline = time.time() + 25
        started = False
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "ignore").strip()
            if text.startswith("--SHOT-BEGIN"):
                # 结束标记可能和尺寸粘在同一行（设备 printf 不带换行时会这样），
                # 用正则只取 w/h 两个整数，别依赖空格切分。
                nums = re.findall(r"\d+", text)
                if len(nums) >= 2:
                    w, h = int(nums[0]), int(nums[1])
                started = True
                continue
            if text == "--SHOT-END--" or text.startswith("--SHOT-ERR"):
                if text.startswith("--SHOT-ERR"):
                    print(f"设备报错：{text}", file=sys.stderr)
                    return 1
                break
            if started and text.startswith("H:"):
                # H:0000:<hex>
                _, idx, payload = text.split(":", 2)
                lines[int(idx)] = bytes.fromhex(payload)

        if not started or not lines:
            print("没抓到画面（设备没响应 p 命令？）", file=sys.stderr)
            return 1
        if w != SHOT_W or h != SHOT_H:
            print(f"意外尺寸 {w}x{h}", file=sys.stderr)
            return 1

        stride = SHOT_W * 2
        # 设备按每块 120 字节切分（ui_capture.c 的 HEX_PER_LINE），不是按行，
        # 所以先把所有块按序号拼成整屏，再切成 480 字节一行。
        full = bytearray(stride * SHOT_H)
        got = 0
        for idx, chunk in lines.items():
            off = idx * 120
            if off + len(chunk) > len(full):
                continue
            full[off:off + len(chunk)] = chunk
            got += len(chunk)
        rows = [bytes(full[y * stride:(y + 1) * stride]) for y in range(SHOT_H)]
        missing = (stride * SHOT_H - got) // 120
        write_png(args.out, rows, SHOT_W, SHOT_H)
        print(f"已保存 {args.out}（{SHOT_W}x{SHOT_H}，缺 {missing} 块 / 共 {len(lines)} 块）")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
