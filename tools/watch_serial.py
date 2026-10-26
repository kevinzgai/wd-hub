"""串口监听：打印固件日志里与提示音相关的行。

重点：open() 之前必须把 dtr/rts 置 False。
否则 ESP32-S3 的自动下载电路会拉低 GPIO0/EN，板子直接被硬复位，
日志时间戳从 0 开始，看起来像"板子反复重启"。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
KEYS = ("pet ", "audio:", "提示音", "done=")

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.5
s.dtr = False          # 关键：不要碰 GPIO0
s.rts = False          # 关键：不要碰 EN
s.open()
s.reset_input_buffer()

t0 = time.time()
print(f"监听 {PORT} {SECONDS:.0f}s ...", flush=True)
while time.time() - t0 < SECONDS:
    try:
        line = s.readline().decode("utf-8", "replace").rstrip()
    except Exception as e:
        print("READ-ERR", e, flush=True)
        break
    if not line:
        continue
    if any(k in line for k in KEYS):
        print(line, flush=True)
s.close()
print("--- 监听结束 ---", flush=True)
