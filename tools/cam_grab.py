"""从串口接收 OV7670 导出的帧，存成 BMP（不存 PPM，Windows 双击就能看）。

用法：
    python tools/cam_grab.py COM4                 # 存 3 帧到 .tmp/cam/
    python tools/cam_grab.py COM4 --frames 1      # 只存 1 帧
    python tools/cam_grab.py COM4 --baud 921600

为什么不用 PPM：Windows 默认没有 .ppm 的看图程序，BMP 双击就能开。

协议（固件 e2-camera 发的）：
    <BMP START w=160 h=120 bytes=38400>\\n
    然后 bytes 个 RGB888 字节，每行 w*3 字节
    </BMP END>\\n

依赖：pyserial（IDF 的 python 环境里就有）
"""

import argparse
import struct
import sys
import time
from pathlib import Path

import serial


def parse_header(line: str):
    """解析 <BMP START w=.. h=.. bytes=..>，返回 (w, h, n) 或 None"""
    if not line.startswith("<BMP START"):
        return None
    kv = {}
    for tok in line.strip("<> \n\r").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = int(v)
    if "w" not in kv or "h" not in kv or "bytes" not in kv:
        return None
    return kv["w"], kv["h"], kv["bytes"]


def write_bmp(path: Path, w: int, h: int, rgb: bytes) -> None:
    """把 RGB888 写成 24 位 BMP（BGR 顺序、行自下而上）"""
    row_size = w * 3
    pad = (4 - row_size % 4) % 4
    pixel_bytes = (row_size + pad) * h
    file_size = 54 + pixel_bytes

    header = b"BM" + struct.pack("<IHHI", file_size, 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII",
                       40,            # biSize
                       w, h,
                       1, 24,         # planes, bitcount
                       0,             # biCompression = BI_RGB
                       pixel_bytes,
                       2835, 2835,    # 72 DPI
                       0, 0)

    body = bytearray()
    for y in range(h - 1, -1, -1):
        off = y * w * 3
        row = rgb[off:off + row_size]
        for i in range(0, len(row), 3):
            body += bytes((row[i + 2], row[i + 1], row[i]))
        body += b"\x00" * pad

    path.write_bytes(header + info + bytes(body))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="串口，例如 COM4")
    ap.add_argument("--baud", type=int, default=921600, help="波特率（要和固件一致）")
    ap.add_argument("--frames", type=int, default=3, help="最多收几帧")
    ap.add_argument("--timeout", type=float, default=40.0, help="总超时秒")
    ap.add_argument("--out", default=r".tmp\cam", help="输出目录")
    args = ap.parse_args()

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.3)
    except Exception as exc:                       # noqa: BLE001
        print(f"[open failed] {type(exc).__name__}: {exc}")
        return 1

    # 复位板子开始新的一轮
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.reset_input_buffer()

    got = 0
    deadline = time.time() + args.timeout
    line_buf = b""

    print(f"[{args.port} @ {args.baud}] 等待帧头 <BMP START ...> …")

    while got < args.frames and time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue

        # 先按行累积，找帧头
        if not line_buf:
            head = chunk[:256]
            if b"<BMP START" not in head:
                line_buf = b""
                print("[日志] " + chunk.decode("utf-8", "replace").rstrip()[:120])
                continue

        # 从 chunk 里定位帧头
        idx = chunk.find(b"<BMP START")
        if idx < 0:
            continue
        nl = chunk.find(b"\n", idx)
        if nl < 0:
            continue
        header_line = chunk[idx:nl].decode("utf-8", "replace")
        parsed = parse_header(header_line)
        if parsed is None:
            continue
        w, h, nbytes = parsed
        print(f"[帧 {got + 1}] {w}x{h}, {nbytes} 字节，接收中…")

        # 收数据（考虑 header 之后同一 chunk 里可能已经有一部分）
        data = bytearray(chunk[nl + 1:])
        while len(data) < nbytes and time.time() < deadline:
            more = ser.read(min(4096, nbytes - len(data)))
            if more:
                data += more
        if len(data) < nbytes:
            print(f"[帧 {got + 1}] 数据不完整（{len(data)}/{nbytes}），跳过")
            continue

        data = data[:nbytes]
        path = outdir / f"frame-{got + 1}.bmp"
        write_bmp(path, w, h, bytes(data))
        print(f"[帧 {got + 1}] 已存 {path}  ({path.stat().st_size} 字节)")
        got += 1

        # 丢掉帧尾标记
        tail = ser.read(64)

    ser.close()
    if got == 0:
        print("[失败] 一帧都没收到。确认：板子已烧 e2-camera、波特率一致、串口号正确")
        return 2
    print(f"[完成] 共 {got} 帧，在 {outdir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
