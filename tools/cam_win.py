#!/usr/bin/env python3
"""cam_win.py —— 开着窗口实时看 OV7670 的真实画面（走数据线，不联网）。

配合 firmware/e3-ref：
    · 推帧默认是关的，必须先发 PUSH 1
    · 帧格式： META,<行字节数>,<宽>,<高>\\n + 裸 RGB565(大端) + \\nEND\\n
    · VGA 640x480 = 614400 字节，921600 波特率约 6.7 秒/帧
      QVGA 320x240 = 153600 字节，约 1.7 秒/帧

用法：
    python tools/cam_win.py                  # 自动找串口，COM4
    python tools/cam_win.py COM4
    python tools/cam_win.py COM4 --scale 2   # 放大两倍显示
    python tools/cam_win.py COM4 --save a.png  # 按 s 存图

窗口里按 q 退出、s 存图。
⚠️ 不要碰 DTR/RTS：这块 CH340 板 RTS 控 EN、DTR 控 GPIO0，
   拉低等于一直按住复位，症状是端口能打开但一个字节都收不到。
"""
import argparse
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
    sys.exit("没有 tkinter，看不了实时窗口（换系统 python 跑）")

META = re.compile(rb"META,(\d+),(\d+),(\d+)\n")


def pick_port(explicit=None):
    if explicit:
        return explicit
    ports = [p.device for p in serial.tools.list_ports.comports()]
    if not ports:
        sys.exit("没找到串口。板子插好了吗？")
    return ports[0]


class Reader(threading.Thread):
    """后台读串口、解帧、转成 PIL 图像。"""

    def __init__(self, port, baud=921600):
        super().__init__(daemon=True)
        self.ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        # 推帧默认关，先开
        self.ser.write(b"PUSH 1\n")
        self.ser.flush()

        self.frame = None          # 最新一帧 PIL Image
        self.count = 0
        self.fps = 0.0
        self.err = None
        self.mean = 0.0
        self.gx = 0.0              # 横梯度（行内细节，真图像几十）
        self.size = (0, 0)
        self._stop = False
        self._t_last = time.time()

    def run(self):
        buf = bytearray()
        try:
            while not self._stop:
                chunk = self.ser.read(65536)
                if not chunk:
                    continue
                buf += chunk
                while True:
                    m = META.search(buf)
                    if not m:
                        if len(buf) > 8_000_000:
                            del buf[:-65536]
                        break
                    row_sz, w, h = (int(x) for x in m.groups())
                    need = row_sz * h
                    if len(buf) < m.end() + need:
                        break
                    raw = bytes(buf[m.end():m.end() + need])
                    del buf[:m.end() + need]
                    self._decode(raw, w, h)
        except Exception as e:                              # noqa: BLE001
            self.err = "%s: %s" % (type(e).__name__, e)

    def _decode(self, raw, w, h):
        a = np.frombuffer(raw, dtype=">u2")[:h * w].reshape(h, w)
        r = ((a >> 11) & 0x1F).astype(np.uint16) << 3
        g = ((a >> 5) & 0x3F).astype(np.uint16) << 2
        b = (a & 0x1F).astype(np.uint16) << 3
        rgb = np.dstack([r, g, b]).astype(np.uint8)
        self.frame = Image.fromarray(rgb, "RGB")
        self.count += 1
        self.size = (w, h)

        lum = (rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114)
        self.mean = float(lum.mean())
        self.gx = float(np.abs(np.diff(lum, axis=1)).mean())

        now = time.time()
        dt = now - self._t_last
        if dt > 0:
            self.fps = 1.0 / dt
        self._t_last = now

    def stop(self):
        self._stop = True
        try:
            self.ser.write(b"PUSH 0\n")
            self.ser.flush()
        except Exception:                                   # noqa: BLE001
            pass
        time.sleep(0.05)
        try:
            self.ser.close()
        except Exception:                                   # noqa: BLE001
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--save", default="cam_shot.png")
    args = ap.parse_args()

    port = pick_port(args.port)
    print("串口 %s @ %d" % (port, args.baud))
    rd = Reader(port, args.baud)
    rd.start()

    root = tk.Tk()
    root.title("OV7670 实时 — %s" % port)
    ph = tk.Label(root, text="等第一帧…（VGA 约 6.7 秒/帧，QVGA 约 1.7 秒）",
                  bg="#111", fg="#ddd", width=60, height=12)
    ph.pack()
    bar = tk.Label(root, text="", anchor="w", bg="#000", fg="#0f0", font=("Consolas", 10))
    bar.pack(fill="x")
    img_ref = {"img": None, "last": -1}

    def tick():
        if rd.err:
            bar.config(text="出错：%s" % rd.err, fg="#f55")
        elif rd.frame is not None and rd.count != img_ref["last"]:
            im = rd.frame
            if args.scale > 1:
                im = im.resize((im.width * args.scale, im.height * args.scale), Image.NEAREST)
            img_ref["img"] = ImageTk.PhotoImage(im)
            ph.config(image=img_ref["img"], text="", width=im.width, height=im.height)
            img_ref["last"] = rd.count
            bar.config(
                text="%dx%d  帧%d  %.2f fps  均值%.0f  行内细节%.2f"
                     % (rd.size[0], rd.size[1], rd.count, rd.fps, rd.mean, rd.gx),
                fg="#0f0")
        root.after(120, tick)

    def on_key(e):
        if e.char == "q":
            root.destroy()
        elif e.char == "s" and rd.frame is not None:
            rd.frame.save(args.save)
            bar.config(text="已存 %s" % args.save, fg="#ff0")

    root.bind("<Key>", on_key)
    root.after(120, tick)
    try:
        root.mainloop()
    finally:
        rd.stop()
        print("关掉了。共收 %d 帧。" % rd.count)


if __name__ == "__main__":
    main()
