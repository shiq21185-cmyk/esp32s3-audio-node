"""e4-ov2640 抓图工具：从串口收 JPEG 帧，直接存成 .jpg。

板子：COM4 @ 921600（CH340，USB-SERIAL CH340 / VID_1A86 PID_7523）
固件：firmware/e4-ov2640 —— 推帧默认关，发 "PUSH 1" 才出图；
      帧格式  META,JPEG,<字节数>\\n + JPEG 原始字节 + \\nEND\\n

用法：
    python -u tools/cam_jpeg.py                    # 抓 3 帧到 .tmp/cam/jpeg/
    python -u tools/cam_jpeg.py -n 1               # 只要一帧
    python -u tools/cam_jpeg.py --probe            # 只看启动日志（不推帧）
    python -u tools/cam_jpeg.py -n 0 --cmd "REG 12"  # 只发命令
    python -u tools/cam_jpeg.py --port COM5 -n 2

注意：
  * 不碰 DTR/RTS —— CH340 上 RTS=EN、DTR=GPIO0，拉低就是一直复位。
  * 用 python -u 跑，否则 os._exit 之类会吞掉输出缓冲。
"""

import argparse
import os
import re
import sys
import time

import serial

META = re.compile(rb"META,JPEG,(\d+)\n")
END = b"\nEND\n"


def open_port(port, baud, timeout=0.5):
    s = serial.Serial(port, baud, timeout=timeout)
    # CH340 上 RTS=EN、DTR=GPIO0。打开串口时把两根都放开（高电平）=
    # 不复位、不进下载模式，板子继续跑现在这版固件。
    try:
        s.rts = False
        s.dtr = False
    except Exception:
        pass
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def read_text(s, quiet=0.4, limit=3.0):
    """收一段文本（启动日志 / 命令回显），非可打印字节用 '.' 顶替。"""
    t0 = last = time.time()
    buf = b""
    while time.time() - t0 < limit:
        ch = s.read(4096)
        if ch:
            buf += ch
            last = time.time()
        elif time.time() - last > quiet:
            break
    return bytes(c if 32 <= c < 127 or c in (10, 13) else 46 for c in buf).decode("ascii", "replace")


def cmd(s, line, quiet=0.3, limit=3.0):
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    s.flush()
    return read_text(s, quiet=quiet, limit=limit)


def grab(s, n=1, timeout=60.0):
    """PUSH 1 -> 收 n 帧 -> PUSH 0。返回 [(len, jpeg_bytes), ...]"""
    cmd(s, "PUSH 1")
    time.sleep(0.2)

    frames, buf = [], bytearray()
    t0 = time.time()
    while len(frames) < n and time.time() - t0 < timeout:
        ch = s.read(65536)
        if not ch:
            continue
        buf += ch
        while len(frames) < n:
            m = META.search(buf)
            if not m:
                # 没找到头：只留尾巴，防止日志把 buffer 撑爆
                if len(buf) > 4_000_000:
                    del buf[:-4096]
                break
            need = int(m.group(1))
            if len(buf) < m.end() + need:
                break
            jpg = bytes(buf[m.end():m.end() + need])
            ok = jpg[:2] == b"\xff\xd8" and jpg[-2:] == b"\xff\xd9"
            if not ok:
                print("  ! 帧头/帧尾不对（SOI/EOI），丢掉重新同步")
                del buf[:m.end() + need]
                continue
            frames.append(jpg)
            del buf[:m.end() + need]
            # 顺带把 "\\nEND\\n" 吃掉
            if buf[:len(END)] == END:
                del buf[:len(END)]
    cmd(s, "PUSH 0")
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM4")
    ap.add_argument("-b", "--baud", type=int, default=1500000)
    ap.add_argument("-n", "--frames", type=int, default=3)
    ap.add_argument("-o", "--out", default=".tmp/cam/jpeg")
    ap.add_argument("--probe", action="store_true", help="只读启动日志，不推帧")
    ap.add_argument("--cmd", action="append", default=[], help="额外命令，可多次给")
    args = ap.parse_args()

    s = open_port(args.port, args.baud)
    try:
        if args.probe:
            print(read_text(s, quiet=1.0, limit=6.0))
            return

        for c in args.cmd:
            print("$ %s" % c)
            print(cmd(s, c))

        if args.frames <= 0:
            return

        t0 = time.time()
        frames = grab(s, args.frames)
        dt = time.time() - t0
        if not frames:
            print("没收到帧。看板子日志：是不是没检测到传感器 / PUSH 没收")
            return

        os.makedirs(args.out, exist_ok=True)
        stamp = time.strftime("%H%M%S")
        for i, jpg in enumerate(frames):
            path = os.path.join(args.out, "ov2640_%s_%d.jpg" % (stamp, i))
            with open(path, "wb") as f:
                f.write(jpg)
            print("  %s  %d 字节 (%.0f KB)" % (path, len(jpg), len(jpg) / 1024.0))
        print("共 %d 帧，用时 %.2f 秒（含命令往返）" % (len(frames), dt))
    finally:
        s.close()


if __name__ == "__main__":
    main()
