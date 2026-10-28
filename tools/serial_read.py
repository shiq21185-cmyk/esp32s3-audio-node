import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

try:
    ser = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:  # noqa: BLE001
    print(f"[open failed] {type(e).__name__}: {e}")
    raise SystemExit(1)

# 让板子从复位开始跑：拉低 DTR/RTS 组合在 ESP32-S3 原生 USB 上等同 EN 复位
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
time.sleep(0.05)
ser.reset_input_buffer()

end = time.time() + dur
buf = ""
while time.time() < end:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk.decode("utf-8", errors="replace")

ser.close()
print(buf if buf.strip() else "<没有收到任何串口数据>")
