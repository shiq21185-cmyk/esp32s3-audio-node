"""e4-ov2640 实时预览：把板子推来的 JPEG 转成浏览器能看的 MJPEG。

板子：COM4 @ 921600（CH340）。固件：firmware/e4-ov2640
      帧格式 META,JPEG,<len>\\n + JPEG + \\nEND\\n，发 PUSH 1 才连续推。

用法：
    python -u tools/cam_stream.py                 # 默认 http://127.0.0.1:8765
    python -u tools/cam_stream.py --http-port 9000

浏览器打开 / 就是实时画面；/snap.jpg 拿最新一帧静态图；/stats 是 JSON。

注意：
  * 串口是独占的 —— 这个脚本跑着的时候，cam_jpeg.py 会打不开 COM4。
  * 不碰 DTR/RTS（CH340 上 RTS=EN、DTR=GPIO0，拉低就是复位）。
"""

import argparse
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

META = re.compile(rb"META,JPEG,(\d+)\n")
HEAD = b"META,JPEG,"
SOI, EOI = b"\xff\xd8", b"\xff\xd9"

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>OV2640 live</title>
<style>
  html,body{margin:0;height:100%%;background:#111;color:#ddd;font:13px/1.4 system-ui,sans-serif}
  .wrap{display:flex;flex-direction:column;height:100%%}
  .bar{padding:8px 12px;background:#1b1b1b;border-bottom:1px solid #333;
       display:flex;gap:16px;align-items:center;font-variant-numeric:tabular-nums}
  .bar b{color:#6cf;font-weight:600}
  .stage{flex:1;display:flex;align-items:center;justify-content:center;overflow:hidden}
  img{max-width:100%%;max-height:100%%;image-rendering:auto}
</style></head>
<body><div class="wrap">
  <div class="bar">
    <b>OV2640</b><span id="fps">-</span><span id="size">-</span>
    <span id="age">-</span><span style="margin-left:auto;color:#777">%s</span>
  </div>
  <div class="stage"><img id="v" src="/stream.mjpg"></div>
</div>
<script>
setInterval(async () => {
  try {
    const s = await (await fetch('/stats')).json();
    document.getElementById('fps').textContent = '帧率 ' + s.fps.toFixed(2) + ' fps';
    document.getElementById('size').textContent = ' ' + (s.bytes/1024).toFixed(1) + ' KB';
    document.getElementById('age').textContent = ' ' + s.age.toFixed(2) + ' 秒前';
  } catch (e) {}
}, 700);
</script>
</body></html>
"""


class FrameSource:
    """开串口、发 PUSH 1、把最新一帧放在 self.frame 里（加锁）。"""

    def __init__(self, port, baud):
        self.port, self.baud = port, baud
        self.lock = threading.Lock()
        self.frame = None
        self.seq = 0
        self.bytes = 0
        self.stamp = 0.0
        self.count = 0
        self.fps = 0.0
        self.error = ""
        self.stop = threading.Event()
        self.ser = None

    def open(self):
        s = serial.Serial(self.port, self.baud, timeout=0.2)
        try:
            s.rts = False   # EN 高 = 不复位
            s.dtr = False   # GPIO0 高 = 正常启动
        except Exception:
            pass
        time.sleep(0.3)
        s.reset_input_buffer()
        self.ser = s
        s.write(b"PUSH 1\n")
        s.flush()

    def run(self):
        buf = bytearray()
        last_count_t = time.time()
        while not self.stop.is_set():
            try:
                if self.ser is None:
                    self.open()
                ch = self.ser.read(65536)
            except Exception as e:                      # 拔线/复位/端口丢了
                self.error = "串口:%s" % e
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(1.0)
                buf.clear()
                continue
            if not ch:
                continue
            buf += ch

            while True:
                m = META.search(buf)
                if not m:
                    # 没有帧头：别让 buffer 无限涨，只留最后一点点
                    if len(buf) > 2_000_000:
                        del buf[:-4096]
                    break
                need = int(m.group(1))
                if len(buf) < m.end() + need:
                    break
                jpg = bytes(buf[m.end():m.end() + need])
                del buf[:m.end() + need]
                if buf[:5] == b"\nEND\n":
                    del buf[:5]
                if jpg[:2] != SOI or jpg[-2:] != EOI:
                    self.error = "帧头/帧尾不对，重新同步"
                    continue
                with self.lock:
                    self.frame = jpg
                    self.seq += 1
                    self.bytes = len(jpg)
                    self.stamp = time.time()
                    self.count += 1
                self.error = ""

            now = time.time()
            if now - last_count_t >= 1.0:
                with self.lock:
                    self.fps = self.count / (now - last_count_t)
                    self.count = 0
                last_count_t = now

    def snapshot(self):
        with self.lock:
            return self.frame, self.seq, self.bytes, self.stamp, self.fps, self.error

    def close(self):
        self.stop.set()
        try:
            if self.ser:
                self.ser.write(b"PUSH 0\n")
                self.ser.flush()
                self.ser.close()
        except Exception:
            pass


SRC = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            body = (PAGE % (SRC.port if SRC else "?")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/snap.jpg":
            jpg, *_ = SRC.snapshot()
            if not jpg:
                self.send_error(404, "no frame yet")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(jpg)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(jpg)
        elif path == "/stats":
            jpg, seq, nbytes, stamp, fps, err = SRC.snapshot()
            body = json.dumps({
                "seq": seq, "bytes": nbytes, "fps": round(fps, 2),
                "age": round(time.time() - stamp, 2) if stamp else -1,
                "error": err,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=--fr")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            last = -1
            try:
                while True:
                    jpg, seq, *_ = SRC.snapshot()
                    if jpg and seq != last:
                        last = seq
                        self.wfile.write(b"--fr\r\nContent-Type: image/jpeg\r\n"
                                         b"Content-Length: %d\r\n\r\n" % len(jpg))
                        self.wfile.write(jpg)
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                    else:
                        time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError):
                return
        else:
            self.send_error(404)


def main():
    global SRC
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM4")
    ap.add_argument("-b", "--baud", type=int, default=1500000)
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    SRC = FrameSource(args.port, args.baud)
    t = threading.Thread(target=SRC.run, daemon=True)
    t.start()

    print("串口 %s @ %d，PUSH 1 已发" % (args.port, args.baud), flush=True)
    print("浏览器打开 http://%s:%d/" % (args.host, args.http_port), flush=True)

    srv = ThreadingHTTPServer((args.host, args.http_port), Handler)
    srv.daemon_threads = True
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        SRC.close()


if __name__ == "__main__":
    main()
