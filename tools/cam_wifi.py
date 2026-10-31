#!/usr/bin/env python3
"""cam_wifi.py —— 收板子推过来的 JPEG，转成本机网页（反向推流接收端）

为什么是"反向"：
    板子在 Wi-Fi 网段（192.168.1.x），这台 PC 在以太网网段（192.168.77.x），
    中间隔着 NAT，PC 主动连 192.168.1.105 是连不上的；
    但板子连得出去（实测 184ms）。所以固件里的 net_push.c 当客户端连到本机，
    本机当服务器收帧，再用 127.0.0.1 的网页给你看 —— 浏览器不用跨网段。

用法：
    python tools/cam_wifi.py                 # 收 8765，网页 8766
    python tools/cam_wifi.py --http-port 8080
    python tools/cam_wifi.py --list-only     # 只收不开网页（看帧率用）

网页：  http://127.0.0.1:8766/
帧率：  http://127.0.0.1:8766/stats
"""

import argparse
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LATEST = {"jpg": None, "ts": 0.0, "seq": 0, "bytes": 0, "src": None}
LOCK = threading.Lock()
STATS = {"fps": 0.0, "seq": 0, "last": 0.0, "clients": 0, "err": ""}

INDEX_HTML = """<!doctype html><html><head><meta charset="utf-8">
<title>OV2640 - Wi-Fi</title>
<style>body{margin:0;background:#111;color:#ddd;font:14px/1.5 system-ui}
#w{display:flex;align-items:center;justify-content:center;height:100vh}
img{max-width:100vw;max-height:100vh}#s{position:fixed;left:8px;top:8px;background:#000a;padding:4px 8px;border-radius:6px}</style>
</head><body><div id="s">等待板子…</div><div id="w"><img src="/stream.mjpg"></div>
<script>setInterval(async()=>{const r=await fetch('/stats');document.getElementById('s').textContent=await r.text()},1000)</script>
</body></html>""".encode("utf-8")


def parse_frames(sock, on_frame):
    """从 TCP 流里切出 `META,JPEG,<n>\\n` + JPEG + `\\nEND\\n`。"""
    buf = bytearray()
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return "连接关闭"
        buf += chunk
        while True:
            if buf[:5] != b"META,":
                i = buf.find(b"META,")
                if i < 0:
                    if len(buf) > 4096:
                        del buf[:-8]
                    break
                del buf[:i]
            nl = buf.find(b"\n")
            if nl < 0:
                break
            head = bytes(buf[:nl])
            try:
                _, kind, n = head.split(b",")
                n = int(n)
            except ValueError:
                del buf[: nl + 1]
                continue
            if len(buf) < nl + 1 + n + 5:
                break
            jpg = bytes(buf[nl + 1 : nl + 1 + n])
            del buf[: nl + 1 + n + 5]
            if jpg[:2] == b"\xff\xd8" and jpg[-2:] == b"\xff\xd9":
                on_frame(jpg)


def handle_client(conn, addr):
    """一条连接一个线程：浏览器的杂连接不能把板子那条挤掉。"""
    print(f"[recv] 连接 {addr[0]}:{addr[1]}", flush=True)
    t0, n0 = time.time(), 0

    def on_frame(jpg):
        nonlocal n0, t0
        now = time.time()
        n0 += 1
        with LOCK:
            LATEST.update(jpg=jpg, ts=now, seq=LATEST["seq"] + 1,
                          bytes=len(jpg), src=addr[0])
            STATS["seq"] = LATEST["seq"]
            dt = now - t0
            if dt >= 1.0:
                STATS["fps"] = (n0 - 1) / dt
                STATS["last"] = len(jpg)
                n0, t0 = 1, now

    try:
        err = parse_frames(conn, on_frame)
    except OSError as e:
        err = f"{type(e).__name__}: {e}"
    finally:
        conn.close()
    if n0:
        STATS["err"] = err
        print(f"[recv] {addr[0]}:{addr[1]} 断开（{err}），{n0} 帧", flush=True)


def accept_loop(port):
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind(("0.0.0.0", port))
    tcp.listen(16)
    print(f"[recv] 等板子连进来：0.0.0.0:{port}", flush=True)

    while True:
        conn, addr = tcp.accept()
        STATS["clients"] += 1
        threading.Thread(target=handle_client, args=(conn, addr),
                         daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._send(200, "text/html; charset=utf-8", INDEX_HTML)
        elif self.path == "/snap.jpg":
            with LOCK:
                jpg = LATEST["jpg"]
            if not jpg:
                self._send(503, "text/plain; charset=utf-8", b"no frame yet")
            else:
                self._send(200, "image/jpeg", jpg)
        elif self.path == "/stats":
            with LOCK:
                age = time.time() - LATEST["ts"] if LATEST["ts"] else -1
                txt = (f"seq={LATEST['seq']} {STATS['fps']:.1f}fps "
                       f"frame={STATS['last']}B age={age:.2f}s "
                       f"src={LATEST['src']} err={STATS['err'] or '-'}")
            self._send(200, "text/plain; charset=utf-8", txt.encode())
        elif self.path.startswith("/stream"):
            self._send_stream()
        else:
            self._send(404, "text/plain", b"404")

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_stream(self):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        last = -1
        try:
            while True:
                with LOCK:
                    seq, jpg = LATEST["seq"], LATEST["jpg"]
                if jpg is None or seq == last:
                    time.sleep(0.005)
                    continue
                last = seq
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                 b"Content-Length: " + str(len(jpg)).encode()
                                 + b"\r\n\r\n" + jpg + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8767, help="收帧端口（板子连这个）")
    ap.add_argument("--http-port", type=int, default=8765, help="网页端口")
    ap.add_argument("--list-only", action="store_true", help="不开网页")
    a = ap.parse_args()

    t = threading.Thread(target=accept_loop, args=(a.port,), daemon=True)
    t.start()

    if a.list_only:
        try:
            while True:
                time.sleep(2)
                with LOCK:
                    if LATEST["ts"]:
                        print(f"[recv] {STATS['fps']:.1f}fps  {STATS['last']}B  "
                              f"共 {LATEST['seq']} 帧", flush=True)
        except KeyboardInterrupt:
            return

    srv = ThreadingHTTPServer(("127.0.0.1", a.http_port), Handler)
    srv.daemon_threads = True
    print(f"[web ] 打开 http://127.0.0.1:{a.http_port}/", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
