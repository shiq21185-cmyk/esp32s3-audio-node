#!/usr/bin/env python3
"""cam_uart_live.py —— e5 身体固件：串口实时看画面（本地，不走云）

为什么单独做一个：
    MQTT 那条路一帧要绕一次云（2~4fps、延迟 1~3s），调镜头焦距时根本分不清
    到底对没对上。这根线在电脑旁边时，串口 1.5M 能到 ~10fps、延迟 <100ms。

协议（e5 固件 CAM UART 1）：
    主机 115200 发 "CAM UART 1"
    板子先回一行 "BAUD 1500000"，然后把 UART0 切到 1.5M，日志全关
    之后每帧： "META,JPEG,<len>\\n" + 裸 JPEG + "\\nEND\\n"
    退出时（Ctrl-C）：重连 1.5M 发 "CAM UART 0"，板子把波特率切回 115200

用法：
    python tools/cam_uart_live.py                 # COM4，网页 http://127.0.0.1:8765/
    python tools/cam_uart_live.py --port COM5 --size VGA --qual 12
    python tools/cam_uart_live.py --no-web        # 只在终端看帧率
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cam_wifi as W  # noqa: E402  复用同一套网页/数据槽

BOOT_BAUD = 115200
META_TAG = b"META,JPEG,"   # 板子每帧的帧头（10 字节）
FAST_BAUD = 1500000


def open_port(port: str, baud: int) -> serial.Serial:
    s = serial.Serial(port, baud, timeout=0.05)
    # CH340：RTS→EN、DTR→GPIO0。拉一下就把板子打回复位/下载模式，所以都放开。
    s.rts = False
    s.dtr = False
    return s


def read_line(s: serial.Serial, timeout: float = 2.0) -> str:
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        buf += s.read(64)
        if b"\n" in buf:
            break
    return buf.decode("utf-8", "replace").strip()


def wait_ready(s: serial.Serial, timeout: float = 25.0) -> bool:
    """等板子把启动日志打完再发命令。

    刚复位时串口上先是 bootloader 和几十行启动日志；这时候发 `CAM UART 1`
    会被日志冲掉（实测：命令丢了，报"没等到 BAUD 行"）。所以先等一条 e5 的日志。
    """
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        buf += s.read(256)
        if b") e5: " in buf:
            time.sleep(0.4)          # 让它把剩下的自检日志吐完
            s.reset_input_buffer()
            return True
    print(f"[uart] 等启动日志超时，最后收到：{buf[-120:]!r}", flush=True)
    return False


def switch_to_fast(s: serial.Serial, size: str | None, qual: int | None) -> bool:
    """115200 下发 CAM UART 1，等板子回 BAUD 1500000。"""
    if size:
        s.write(f"CAM SIZE {size}\n".encode())
        time.sleep(0.6)
    if qual is not None:
        s.write(f"CAM QUAL {qual}\n".encode())
        time.sleep(0.4)
    s.write(b"CAM UART 1\n")
    s.flush()

    t0 = time.time()
    buf = b""
    while time.time() - t0 < 3.0:
        buf += s.read(256)
        if b"BAUD 1500000" in buf:
            return True
    print(f"[uart] 没等到 BAUD 行，收到：{buf[-120:]!r}", flush=True)
    return False


def switch_back(port: str) -> None:
    """尽力而为：把板子从串口推流/1.5M 模式拉回来。"""
    try:
        s = serial.Serial(port, FAST_BAUD, timeout=0.1)
        s.rts = False
        s.dtr = False
        time.sleep(0.2)
        for _ in range(3):
            s.write(b"CAM UART 0\n")
            s.flush()
            time.sleep(0.3)
        s.close()
        print("[uart] 已发 CAM UART 0（板子波特率回到 115200）", flush=True)
    except Exception as e:                                     # noqa: BLE001
        print(f"[uart] 收尾失败（板子可能还在 1.5M 模式）：{e}", flush=True)


def frames(s: serial.Serial):
    """从串口流里切出 JPEG。返回 (jpeg_bytes, len_field)。"""
    buf = bytearray()
    idle = 0
    while True:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            idle = 0
        else:
            idle += 1
            if idle % 200 == 0:                     # 10 秒还没料，说一声
                print(f"[uart] 等帧中… 缓冲 {len(buf)}B", flush=True)
        head = buf.find(b"META,JPEG,")
        if head < 0:
            # 丢掉垃圾，但留一点尾巴防止标记被切断
            if len(buf) > 65536:
                del buf[: len(buf) - 32]
            continue
        if head:
            del buf[:head]
        nl = buf.find(b"\n")
        if nl < 0:
            if len(buf) > 64:
                del buf[: len(buf) - 32]           # 防止坏行把缓冲撑爆
            continue
        try:
            # "META,JPEG," 是 10 个字符 —— 之前写成 8，把 "G," 也塞进了 int()，
            # 结果每帧都 ValueError 被当坏帧丢掉，表面上像"板子没出帧"。
            n = int(bytes(buf[len(META_TAG):nl]).strip())
        except ValueError:
            del buf[: nl + 1]
            continue
        need = nl + 1 + n
        if len(buf) < need:
            continue
        data = bytes(buf[nl + 1: need])
        del buf[:need]
        if data[:2] != b"\xff\xd8":
            print("[uart] 帧头不是 FFD8，跳过", flush=True)
            continue
        yield data, n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--size", default=None, help="VGA / QVGA")
    ap.add_argument("--qual", type=int, default=None, help="JPEG 质量 0-63")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--no-web", action="store_true")
    a = ap.parse_args()

    print(f"[uart] 打开 {a.port} @ {BOOT_BAUD}", flush=True)
    s = open_port(a.port, BOOT_BAUD)
    if not wait_ready(s):
        s.close()
        return 2
    if not switch_to_fast(s, a.size, a.qual):
        s.close()
        return 2

    print(f"[uart] 板子已切到 {FAST_BAUD}，开始收帧", flush=True)
    s.baudrate = FAST_BAUD
    time.sleep(0.3)
    s.reset_input_buffer()

    stop = threading.Event()

    def pump():
        n, t0 = 0, time.time()
        for data, _declared in frames(s):
            now = time.time()
            n += 1
            dt = now - t0
            if dt >= 1.0:
                W.STATS["fps"] = (n - 1) / dt
                W.STATS["last"] = len(data)
                n, t0 = 1, now
            W.LATEST.update(jpg=data, ts=now, seq=W.LATEST["seq"] + 1,
                            bytes=len(data), src="uart")
            W.STATS["seq"] = W.LATEST["seq"]
            W.STATS["err"] = ""
            if stop.is_set():
                return

    th = threading.Thread(target=pump, daemon=True)
    th.start()

    try:
        if a.no_web:
            while True:
                time.sleep(2)
                if W.LATEST["ts"]:
                    print(f"[uart] {W.STATS['fps']:.1f}fps  {W.STATS['last']}B  "
                          f"共 {W.LATEST['seq']} 帧", flush=True)
        from http.server import ThreadingHTTPServer
        srv = ThreadingHTTPServer(("127.0.0.1", a.http_port), W.Handler)
        srv.daemon_threads = True
        print(f"[web ] http://127.0.0.1:{a.http_port}/", flush=True)
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        try:
            s.close()
        except Exception:                                      # noqa: BLE001
            pass
        switch_back(a.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
