#!/usr/bin/env python3
"""cam_mqtt.py —— 从 MQTT 收板子推的 JPEG，转成本机网页看（跨网看图）

协议（和 e2-camera 那套一致）：
    板子 → PC   <base>/cam/jpeg   一帧一张 JPEG（二进制）
    PC → 板子   <base>/cam/cmd    "STREAM 1|0" / "SNAP" / "RATE 200" / "BAR 1|0"

为什么走 MQTT：板子在 Wi-Fi 网段（192.168.1.x），这台电脑在以太网网段
（192.168.77.x），中间隔着 NAT —— 直连不通，两边都往 EMQX 做出站连接最省事。

用法：
    python tools/cam_mqtt.py --stream 1          # 开流 + 开网页
    python tools/cam_mqtt.py --stream 1 --no-web # 只在终端看帧率
    python tools/cam_mqtt.py --snap --save .tmp/cam/snap.jpg
    python tools/cam_mqtt.py --stream 0          # 关流（省 EMQX 流量）

网页：http://127.0.0.1:8765/    帧率：http://127.0.0.1:8765/stats
"""

from __future__ import annotations

import argparse
import ssl
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cam_wifi as W  # noqa: E402  复用同一个网页/数据槽

DEFAULT_BROKER = "i1e78188.ala.cn-hangzhou.emqxsl.cn"
DEFAULT_PORT = 8883
DEFAULT_USER = "whale"
DEFAULT_PASS = "REDACTED-SEE-CONFIG"
DEFAULT_BASE = "whale-8f2a5c31"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default=DEFAULT_BROKER)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--user", default=DEFAULT_USER)
    ap.add_argument("--password", default=DEFAULT_PASS)
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--stream", type=int, default=None,
                    help="连上后发 STREAM 1 或 0")
    ap.add_argument("--rate", type=int, default=None, help="发 RATE <ms>")
    ap.add_argument("--snap", action="store_true", help="只抓一张存盘")
    ap.add_argument("--save", default=r".tmp\cam\mqtt_snap.jpg")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--no-web", action="store_true")
    ap.add_argument("--frames", type=int, default=0, help="收够这么多帧就退出")
    a = ap.parse_args()

    topic_jpeg = f"{a.base}/cam/jpeg"
    topic_cmd = f"{a.base}/cam/cmd"

    cli = mqtt.Client(client_id=f"camview-{int(time.time()) % 100000}",
                      callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    cli.username_pw_set(a.user, a.password or "")
    cli.tls_set(cert_reqs=ssl.CERT_REQUIRED)

    state = {"n": 0, "t0": 0.0, "got": 0}

    def on_connect(c, u, flags, rc, props=None):
        print(f"[MQTT] 已连 broker（{rc}），订阅 {topic_jpeg}", flush=True)
        c.subscribe(topic_jpeg, qos=0)
        if a.snap:
            c.publish(topic_cmd, "SNAP", qos=0)
        elif a.stream is not None:
            # QoS0 不保留：板子那一侧还没订阅完的话，这条会直接丢。
            # 所以连发几轮，直到第一帧到了为止（后台边收边发，见 main 里）。
            for _ in range(3):
                if a.rate is not None:
                    c.publish(topic_cmd, f"RATE {a.rate}", qos=0)
                c.publish(topic_cmd, f"STREAM {a.stream}", qos=0)
                time.sleep(0.4)

    def on_message(c, u, msg):
        data = msg.payload
        if len(data) < 4 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
            print(f"[MQTT] 收到坏帧 {len(data)} 字节", flush=True)
            return
        now = time.time()
        state["got"] += 1
        if not state["t0"]:
            state["t0"] = now
        state["n"] += 1
        dt = now - state["t0"]
        if dt >= 1.0:
            W.STATS["fps"] = (state["n"] - 1) / dt
            W.STATS["last"] = len(data)
            state["n"], state["t0"] = 1, now
        W.LATEST.update(jpg=data, ts=now, seq=W.LATEST["seq"] + 1,
                        bytes=len(data), src="mqtt")
        W.STATS["seq"] = W.LATEST["seq"]
        W.STATS["err"] = ""
        if a.snap and state["got"] == 1:
            Path(a.save).parent.mkdir(parents=True, exist_ok=True)
            Path(a.save).write_bytes(data)
            print(f"[MQTT] 已存 {a.save}（{len(data)} 字节）", flush=True)
        if a.frames and state["got"] >= a.frames:
            c.disconnect()

    cli.on_connect = on_connect
    cli.on_message = on_message

    print(f"[MQTT] 连 {a.broker}:{a.port} ...", flush=True)
    cli.connect(a.broker, a.port, keepalive=30)
    cli.loop_start()

    if a.snap:
        t0 = time.time()
        while state["got"] == 0 and time.time() - t0 < 15:
            time.sleep(0.1)
        cli.loop_stop()
        return 0 if state["got"] else 1

    def tick():
        while True:
            time.sleep(2)
            if W.LATEST["ts"]:
                print(f"[MQTT] {W.STATS['fps']:.1f}fps  {W.STATS['last']}B  "
                      f"共 {W.LATEST['seq']} 帧", flush=True)

    if a.no_web or a.frames:
        import threading
        threading.Thread(target=tick, daemon=True).start()
        t0 = time.time()
        while (a.frames and state["got"] < a.frames
               and time.time() - t0 < 60):
            time.sleep(0.2)
        cli.loop_stop()
        return 0

    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", a.http_port), W.Handler)
    srv.daemon_threads = True
    print(f"[web ] http://127.0.0.1:{a.http_port}/", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    cli.loop_stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
