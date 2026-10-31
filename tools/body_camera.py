#!/usr/bin/env python3
"""body_camera.py —— 让板子拍一张（或几张）照片，存到本机，把路径打印出来。

这是"给身体装眼睛"的 PC 侧入口：不依赖串口，走 MQTT（和她的声音同一条链路），
所以板子插在别的机器上、或者在另一个网段，都能拍。

    python -u tools/body_camera.py                  # 拍一张
    python -u tools/body_camera.py -n 3             # 连拍三张（间隔 0.6s）
    python -u tools/body_camera.py --open           # 拍完用系统看图器打开
    python -u tools/body_camera.py --out D:\\x       # 换目录
    python -u tools/body_camera.py --json           # 只打印 JSON（给程序调用）

协议：对上两种固件都兼容。
    e4-ov2640（纯摄像头）：<base>/cam/cmd 收 "SNAP"，发到 <base>/cam/jpeg
    e5-body-cam（身体+摄像头）：<base>/cmd 收 "CAM SNAP"，同样发 <base>/cam/jpeg
    ⇒ 这里两边都发一遍，谁在听谁响应，多发的那个没人订阅、不会有害。

失败时返回码 1（超时没收到）、2（连不上 broker），把原因打在 stderr。
"""

from __future__ import annotations

import argparse
import json
import ssl
import subprocess
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent))
import body_config  # noqa: E402  连接参数只从这里取，别在别处再抄一份

ROT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROT / ".tmp" / "cam"


def is_jpeg(data: bytes) -> bool:
    return len(data) > 4 and data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"


def main() -> int:
    ap = argparse.ArgumentParser(description="让板子拍一张照片")
    ap.add_argument("--base", default=None, help="主题前缀（默认取 body-config）")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="存图目录")
    ap.add_argument("-n", "--count", type=int, default=1, help="拍几张（默认 1）")
    ap.add_argument("--gap", type=float, default=0.6, help="连拍间隔秒")
    ap.add_argument("--timeout", type=float, default=12.0, help="总共等多久")
    ap.add_argument("--open", action="store_true", help="拍完用系统看图器打开")
    ap.add_argument("--json", action="store_true", help="只输出 JSON")
    a = ap.parse_args()

    kw = body_config.link_kwargs()
    # 不带 --base 时，两条板的前缀都试：d8 的（控制台配置里那个）+ e1/e5 面包板的。
    # 这样"随手拍一张"不用先问是哪块板在线。
    if a.base:
        bases = [a.base]
    else:
        bases = [kw["base"]]
        if "whale-8f2a5c31" not in bases:
            bases.append("whale-8f2a5c31")
    base = bases[0] if len(bases) == 1 else " + ".join(bases)
    broker, port = kw["broker"], kw["port"]
    topic_jpegs = [f"{b}/cam/jpeg" for b in bases]
    topic_cams = [f"{b}/cam/cmd" for b in bases]    # e4 听这个
    topic_bodies = [f"{b}/cmd" for b in bases]      # e5 听这个

    outdir = Path(a.out)
    outdir.mkdir(parents=True, exist_ok=True)

    got: list[Path] = []
    errors: list[str] = []
    done = {"n": 0}
    t_start = time.time()

    cli = mqtt.Client(client_id=f"bodycam-{int(time.time() * 1000) % 1000000}",
                      callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    if kw.get("user"):
        cli.username_pw_set(kw["user"], kw.get("password") or "")
    if kw["use_tls"]:
        cli.tls_set(cert_reqs=ssl.CERT_REQUIRED)

    def send_snap():
        # QoS0 不保留：板子还没订阅完的话这条会直接丢，所以两边都发、多等一会儿。
        for t in topic_cams:
            cli.publish(t, "SNAP", qos=0)
        for t in topic_bodies:
            cli.publish(t, "CAM SNAP", qos=0)

    def on_connect(c, u, flags, rc, props=None):
        for t in topic_jpegs:
            c.subscribe(t, qos=0)
        send_snap()

    def on_message(c, u, msg):
        data = msg.payload
        if not is_jpeg(data):
            errors.append(f"坏帧 {len(data)} 字节")
            return
        ts = time.strftime("%Y%m%d_%H%M%S")
        path = outdir / f"body_{ts}_{done['n']:02d}.jpg"
        path.write_bytes(data)
        got.append(path)
        done["n"] += 1
        if not a.json:
            print(f"[cam] {path}  ({len(data)} 字节)", flush=True)
        if done["n"] >= a.count:
            c.disconnect()

    cli.on_connect = on_connect
    cli.on_message = on_message

    if not a.json:
        print(f"[cam] 连 {broker}:{port}，前缀 {base}", flush=True)
    try:
        cli.connect(broker, port, keepalive=30)
    except Exception as e:  # noqa: BLE001
        print(f"[cam] 连不上 broker：{e}", file=sys.stderr)
        return 2
    cli.loop_start()

    last_send = time.time()
    while time.time() - t_start < a.timeout and done["n"] < a.count:
        time.sleep(0.1)
        # 每 1.5 秒重发一次请求（覆盖板子重连 / 订阅晚到 / 单张丢失）
        if time.time() - last_send > 1.5:
            remaining = a.count - done["n"]
            for _ in range(min(remaining, 2)):
                send_snap()
            last_send = time.time()
    cli.loop_stop()

    if a.json:
        print(json.dumps({
            "ok": bool(got),
            "base": base,
            "bases": bases,
            "files": [str(p) for p in got],
            "errors": errors[-5:],
        }, ensure_ascii=False))
    else:
        for err in errors[-5:]:
            print(f"[cam] 丢包：{err}", file=sys.stderr)
        if not got:
            print("[cam] 超时：没收到任何 JPEG。检查板子上电、"
                  "主题前缀一致、以及板子固件是 e4/e5（不是纯音频的 e1）",
                  file=sys.stderr)

    if got and a.open:
        try:
            subprocess.Popen(["cmd", "/c", "start", "", str(got[0])], close_fds=True)
        except Exception:
            pass

    return 0 if got else 1


if __name__ == "__main__":
    raise SystemExit(main())
