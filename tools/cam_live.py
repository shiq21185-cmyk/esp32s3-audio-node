#!/usr/bin/env python3
"""cam_live.py —— 直接插 USB 线，实时看 OV7670 的画面（不走网络）。

板子固件每秒往 UART0 推一帧 JPEG：
    <JPG START bytes=N>\\r\\n  +  JPEG  +  </JPG END>\\r\\n

这个脚本收帧、解 JPEG、弹一个窗口一直刷新。要退出就按 q 或关窗口。

用法：
    python tools/cam_live.py                 # 自动找唯一串口
    python tools/cam_live.py COM4            # 指定端口
    python tools/cam_live.py COM4 --scale 2  # 放大 2 倍显示

依赖：pyserial + Pillow + numpy（系统 python 就有，缺 pyserial 就
      python -m pip install pyserial）。
"""
import argparse
import io
import re
import sys
import threading
import time

import numpy as np
import serial
import serial.tools.list_ports
from PIL import Image, ImageDraw, ImageTk

try:
    import tkinter as tk
except ImportError:
    sys.exit("没有 tkinter，看不了实时窗口")

HDR = re.compile(rb"<JPG START bytes=(\d+)>")


def pick_port(explicit=None):
    if explicit:
        return explicit
    ports = [p.device for p in serial.tools.list_ports.comports()]
    if len(ports) == 1:
        return ports[0]
    if not ports:
        sys.exit("没找到任何串口 —— 板子插上了吗？（设备管理器看 CH340）")
    sys.exit("有多个串口，请指定一个：%s" % " ".join(ports))


class Reader(threading.Thread):
    """后台收帧。串口里混着日志文本和 JPEG 二进制，按帧头切。"""

    def __init__(self, port, baud=115200):
        super().__init__(daemon=True)
        self.ser = serial.Serial(port, baud, timeout=1)
        self.port = port
        self.latest = None          # 最新一帧的 PIL Image
        self.raw = None             # 最新一帧的原始字节
        self.count = 0
        self.fps = 0.0
        self.err = None
        self.buf = bytearray()
        self._t0 = time.time()
        self._n0 = 0
        self.alive = True

    def run(self):
        while self.alive:
            try:
                chunk = self.ser.read(4096)
            except Exception as e:                      # 串口被拔
                self.err = str(e)
                return
            if not chunk:
                continue
            self.buf += chunk
            while True:
                m = HDR.search(self.buf)
                if not m:
                    # 别让缓冲区无限涨（日志很多）
                    if len(self.buf) > 65536:
                        del self.buf[:-4096]
                    break
                need = int(m.group(1))
                start = m.end()
                # 帧头后面跳过所有 CR/LF，否则 JPEG 整体错一位
                while start < len(self.buf) and self.buf[start] in (10, 13):
                    start += 1
                if len(self.buf) < start + need:
                    break                                # 还没收全
                jpg = bytes(self.buf[start:start + need])
                del self.buf[:start + need]
                if not jpg.startswith(b"\xff\xd8"):
                    continue
                try:
                    im = Image.open(io.BytesIO(jpg))
                    im.load()
                except Exception:
                    continue
                self.latest = im.convert("RGB")
                self.raw = jpg
                self.count += 1
                now = time.time()
                if now - self._t0 >= 1.0:
                    self.fps = (self.count - self._n0) / (now - self._t0)
                    self._t0, self._n0 = now, self.count

    def stop(self):
        self.alive = False
        try:
            self.ser.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="串口，例如 COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scale", type=int, default=2, help="显示放大倍数")
    ap.add_argument("--save", action="store_true", help="按 s 存图（配合窗口）")
    args = ap.parse_args()

    port = pick_port(args.port)
    rd = Reader(port, args.baud)
    rd.start()

    root = tk.Tk()
    root.title("OV7670 实时 — %s" % port)
    root.configure(bg="#111")
    label = tk.Label(root, bg="#111")
    label.pack()
    info = tk.Label(root, text="等第一帧…", fg="#ccc", bg="#111",
                    font=("Consolas", 10), anchor="w")
    info.pack(fill="x")

    state = {"img": None, "size": (0, 0), "saved": 0}

    def tick():
        if rd.err:
            info.config(text="串口出错：%s" % rd.err, fg="#f66")
            return
        im = rd.latest
        if im is not None and (im.width, im.height) != state["size"]:
            state["size"] = (im.width, im.height)
            state["img"] = ImageTk.PhotoImage(
                im.resize((im.width * args.scale, im.height * args.scale),
                          Image.NEAREST))
            label.config(image=state["img"])
            root.geometry("")                            # 让窗口自己缩到合适
        a = np.asarray(im.convert("L")).astype(np.int16) if im is not None else None
        if a is not None:
            hd = np.abs(np.diff(a, axis=1))
            text = ("%s  %dx%d  帧 %d  %.1f fps   均值 %.0f  横向差异中位 %.1f  亮度峰 %d"
                    % (port, im.width, im.height, rd.count, rd.fps,
                       a.mean(), np.median(hd), a.max()))
            info.config(text=text)
        root.after(120, tick)

    def on_key(e):
        if e.char == "q":
            rd.stop()
            root.destroy()
        elif e.char == "s" and rd.raw:
            state["saved"] += 1
            name = "cam_%s_%02d.jpg" % (time.strftime("%H%M%S"), state["saved"])
            open(name, "wb").write(rd.raw)
            info.config(text="已存 %s" % name)

    root.bind("<Key>", on_key)
    root.after(120, tick)
    try:
        root.mainloop()
    finally:
        rd.stop()


if __name__ == "__main__":
    main()
