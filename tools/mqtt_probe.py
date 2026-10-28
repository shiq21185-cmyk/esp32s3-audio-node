"""PC 侧 MQTT 探针。

两个用途：
  1. 看板子在发什么（它的状态包）
  2. 给板子发指令，验证"不靠局域网也能通"

用法：
  python mqtt_probe.py --base whale-a90c6c --listen 15
  python mqtt_probe.py --base whale-a90c6c --listen 10 --send '{"act":"anim","name":"happy"}'

默认走自家的 EMQX Serverless（TLS 8883，账号在 body_mqtt.py 里）。
"""

from __future__ import annotations

import argparse
import time

import paho.mqtt.client as mqtt

import body_mqtt


def make_client() -> mqtt.Client:
    """兼容 paho-mqtt 1.x 和 2.x 的构造方式。"""
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        return mqtt.Client()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default=body_mqtt.DEFAULT_BROKER)
    ap.add_argument("--port", type=int, default=body_mqtt.DEFAULT_PORT)
    ap.add_argument("--base", required=True, help="板子的主题前缀，例如 whale-a90c6c")
    ap.add_argument("--listen", type=float, default=10.0, help="听多少秒")
    ap.add_argument("--send", action="append", default=[], help="要发给板子的消息，可多次")
    ap.add_argument("--send-delay", type=float, default=2.0, help="连上后等几秒再发")
    args = ap.parse_args()

    state: dict[str, float] = {"t0": time.time(), "connected_at": 0.0}
    seen = {"n": 0}

    cli = body_mqtt.configure_client(make_client())

    def on_connect(c, userdata, flags, rc, *extra):
        code = rc if isinstance(rc, int) else getattr(rc, "value", rc)
        if code != 0:
            print(f"[连不上] rc={rc}")
            return
        state["connected_at"] = time.time() - state["t0"]
        print(f"[已连上] {args.broker}:{args.port}  耗时 {state['connected_at']:.2f}s")
        c.subscribe(f"{args.base}/#", qos=0)
        print(f"[订阅] {args.base}/#")

    def on_message(c, userdata, msg):
        seen["n"] += 1
        payload = msg.payload.decode("utf-8", "replace")
        print(f"  <- [{msg.topic}] {payload}")

    cli.on_connect = on_connect
    cli.on_message = on_message

    cli.connect(args.broker, args.port, keepalive=30)
    cli.loop_start()

    # 等连上
    deadline = time.time() + 8
    while not state["connected_at"] and time.time() < deadline:
        time.sleep(0.05)
    if not state["connected_at"]:
        print("[失败] 8 秒内没连上 broker")
        cli.loop_stop()
        return 1

    if args.send:
        time.sleep(args.send_delay)
        for text in args.send:
            info = cli.publish(f"{args.base}/cmd", text, qos=0)
            info.wait_for_publish(timeout=3)
            print(f"  -> [{args.base}/cmd] {text}")

    start = time.time()
    while time.time() - start < args.listen:
        time.sleep(0.1)

    print(f"[结束] 共收到 {seen['n']} 条消息")
    cli.loop_stop()
    cli.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
