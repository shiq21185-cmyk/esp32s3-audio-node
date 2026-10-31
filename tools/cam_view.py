"""电脑端：从 MQTT 收 OV7670 的画面（e2-camera 固件发的）。

为什么走 MQTT：电脑在实验室有线网（192.168.77.x），板子在室内 WiFi（192.168.1.x），
两个网段不通（实测 ping 100% 丢包）。MQTT 两边都往外连 broker，跨网没问题。

用法（用她的 venv，里面装了 paho-mqtt）：
    # 抓一张（发 SNAP 给板子）
    python tools/cam_view.py --snap --save .tmp/cam/live.jpg

    # 一直看：开流 + 每收到一帧就覆盖同一个文件，图片查看器会自动刷新
    python tools/cam_view.py --loop --save .tmp/cam/live.jpg

    # 只被动收（板子已经在推流时用）
    python tools/cam_view.py --recv --save .tmp/cam/live.jpg

    # 开关内置彩条（验证数据线用）
    python tools/cam_view.py --bar 1
    python tools/cam_view.py --bar 0
"""

import argparse
import os
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt

DEFAULT_BROKER = "i1e78188.ala.cn-hangzhou.emqxsl.cn"
DEFAULT_PORT = 8883
# 账号密码不写死在源码里（这个仓库是公开的）：优先命令行参数，
# 其次环境变量 BODY_MQTT_USER / BODY_MQTT_PASS，最后读 tools/body_config.py 的配置。
def _default_creds():
    try:
        import body_config
        return body_config.credentials()
    except Exception:
        return (os.environ.get("BODY_MQTT_USER", ""),
                os.environ.get("BODY_MQTT_PASS", ""))


_DEF_USER, _DEF_PASS = _default_creds()
DEFAULT_BASE = "whale-8f2a5c31"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default=DEFAULT_BROKER)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--user", default=_DEF_USER)
    ap.add_argument("--password", default=_DEF_PASS)
    ap.add_argument("--base", default=DEFAULT_BASE, help="主题前缀，要和固件一致")
    ap.add_argument("--save", default=r".tmp\cam\live.jpg", help="存到哪个文件")
    ap.add_argument("--snap", action="store_true", help="发 SNAP 抓一张")
    ap.add_argument("--loop", action="store_true", help="开流并一直收（覆盖同一个文件）")
    ap.add_argument("--recv", action="store_true", help="只被动接收，不发指令")
    ap.add_argument("--bar", choices=["0", "1"], help="开关传感器内置彩条")
    ap.add_argument("--count", type=int, default=0, help="收到几帧后退出（0=不限）")
    ap.add_argument("--timeout", type=float, default=0.0, help="总超时秒（0=不限）")
    args = ap.parse_args()

    topic_jpeg = f"{args.base}/cam/jpeg"
    topic_cmd = f"{args.base}/cam/cmd"

    save_path = Path(args.save)
    save_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = save_path.with_suffix(save_path.suffix + ".part")

    got = 0
    started = time.time()
    first = [True]

    def on_connect(cli, userdata, flags, rc, props=None):
        rcv = int(getattr(rc, "value", rc))
        if rcv != 0:
            print(f"[MQTT] 连不上，reason={rcv}")
            return
        print(f"[MQTT] 已连 {args.broker}:{args.port}")
        cli.subscribe(topic_jpeg, qos=0)
        print(f"[MQTT] 订阅 {topic_jpeg}")

        if args.bar is not None:
            cli.publish(topic_cmd, f"BAR {args.bar}", qos=0)
            print(f"[MQTT] 已发 BAR {args.bar}")
        if args.loop:
            cli.publish(topic_cmd, "STREAM 1", qos=0)
            print("[MQTT] 已发 STREAM 1（开流）")
        elif args.snap:
            cli.publish(topic_cmd, "SNAP", qos=0)
            print("[MQTT] 已发 SNAP（抓一张）")

    def on_message(cli, userdata, msg):
        got_ = len(msg.payload)
        # 先写 .part 再改名，避免图片查看器读到写了一半的文件
        tmp_path.write_bytes(msg.payload)
        tmp_path.replace(save_path)
        if first[0]:
            first[0] = False
            print(f"[收到] {got_} 字节 → {save_path.resolve()}")
            print("        打开它看一眼；--loop 模式下会自动刷新")
        else:
            if got % 20 == 0:
                el = time.time() - started
                print(f"[收到] 第 {got} 帧，{got_} 字节，平均 {got / el:.1f} fps")

    cli = mqtt.Client(client_id=f"camview-{int(time.time()) % 100000}",
                      callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    cli.username_pw_set(args.user, args.password)
    cli.tls_set()
    cli.on_connect = on_connect
    cli.on_message = on_message

    try:
        cli.connect(args.broker, args.port, keepalive=30)
    except Exception as exc:                      # noqa: BLE001
        print(f"[失败] 连不上 broker: {type(exc).__name__}: {exc}")
        return 1

    cli.loop_start()
    try:
        while True:
            time.sleep(0.1)
            if args.count and got >= args.count:
                break
            if args.timeout and (time.time() - started) > args.timeout:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if args.loop:
            cli.publish(topic_cmd, "STREAM 0", qos=0)
            print("[MQTT] 已发 STREAM 0（关流）")
        time.sleep(0.3)
        cli.loop_stop()
        cli.disconnect()
        if tmp_path.exists():
            tmp_path.unlink(missing_ok=True)

    if got == 0:
        print("[失败] 一帧都没收到。确认：板子通电且串口打出 'MQTT 连上了'、主题前缀一致")
        return 2
    print(f"[完成] 共 {got} 帧，最后一张在 {save_path.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
