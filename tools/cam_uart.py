"""通过数据线（UART）从 OV7670 收画面，存成 JPEG。

不需要网络、不需要 MQTT —— 只要板子插着 USB 线就行。这是最可靠的调试通路。

用法（用 IDF 带的 python 就行，只依赖 pyserial）：
    python tools/cam_uart.py COM4                        # 收 1 帧到 .tmp/cam/live.jpg
    python tools/cam_uart.py COM4 --count 5              # 收 5 帧
    python tools/cam_uart.py COM4 --loop                 # 一直收（多帧存成 live-1.jpg…）
    python tools/cam_uart.py COM4 --loop --same-file      # 一直收，覆盖同一个文件（看图器自动刷新）

固件发的格式：
    <JPG START bytes=N>\\n   + N 个字节 JPEG   +   </JPG END>\\n
"""

import argparse
import re
import sys
import time
from pathlib import Path

import serial

HEAD_RE = re.compile(rb"<JPG START bytes=(\d+)>")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--count", type=int, default=1, help="收几帧（0=不限）")
    ap.add_argument("--loop", action="store_true", help="一直收")
    ap.add_argument("--same-file", action="store_true",
                    help="所有帧都写同一个文件（配合图片查看器自动刷新）")
    ap.add_argument("--out", default=r".tmp\cam\live.jpg")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.3)
    except Exception as exc:                       # noqa: BLE001
        print(f"[open failed] {type(exc).__name__}: {exc}")
        return 1

    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.reset_input_buffer()

    print(f"[{args.port} @ {args.baud}] 等第一帧（板子每秒发一帧）…")

    want = 0 if args.loop else args.count
    got = 0
    deadline = time.time() + args.timeout
    buf = bytearray()

    while (want == 0 or got < want) and time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue
        buf += chunk

        m = HEAD_RE.search(buf)
        if not m:
            # 丢掉已经扫过的垃圾，只保留可能不完整的帧头
            if len(buf) > 4096:
                del buf[:-64]
            continue

        n = int(m.group(1))
        # uart 会在 \n 前加 \r，所以跳过所有 CR/LF
        start = m.end()
        while start < len(buf) and buf[start] in (0x0D, 0x0A):
            start += 1
        if len(buf) < start + n:
            continue                             # 数据还没齐，继续收

        jpg = bytes(buf[start:start + n])
        del buf[:start + n]

        if not jpg.startswith(b"\xff\xd8"):
            print(f"[跳过] 不是 JPEG（开头 {jpg[:2].hex()}）")
            continue

        if args.same_file or want == 1:
            target = out
        else:
            target = out.with_name(f"{out.stem}-{got + 1}{out.suffix}")

        tmp = target.with_suffix(target.suffix + ".part")
        tmp.write_bytes(jpg)
        tmp.replace(target)

        got += 1
        print(f"[帧 {got}] {n} 字节 → {target}")

    ser.close()

    if got == 0:
        print("[失败] 一帧都没收到。确认：板子已烧 e2-camera、串口没被别的程序占用")
        return 2
    print(f"[完成] 共 {got} 帧，在 {out.parent.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
