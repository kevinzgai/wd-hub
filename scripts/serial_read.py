#!/usr/bin/env python
"""Read serial output from the board for N seconds and print it.

Note: the board uses the ESP32-S3 built-in USB-Serial/JTAG CDC, which does
not respond to DTR/RTS line toggling the way a real UART bridge does --
touching them raises 'ClearCommError failed (PermissionError)'. So we just
open the port and read.
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.3
ser.dtr = False
ser.rts = False
ser.open()

end = time.time() + secs
buf = b""
while time.time() < end:
    try:
        chunk = ser.read(4096)
    except serial.SerialException as e:
        sys.stderr.write("read error: %r\n" % (e,))
        time.sleep(0.2)
        continue
    if chunk:
        buf += chunk
try:
    ser.close()
except Exception:
    pass

sys.stdout.write(buf.decode("utf-8", errors="replace"))
