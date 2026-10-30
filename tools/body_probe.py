#!/usr/bin/env python
"""板子在不在线？—— 给一键启动用的探活。

以前探活是往板子发 UDP PING。声音搬上 MQTT 之后板子不在同一个网里了，
UDP 这条路整个不通，改成"看 broker 上有没有它的心跳"：
板子平时每 30 秒往 <base>/state 发一条心跳 JSON（状态一变就立刻补一条），
所以这里连上后先发一个 PING 催它立刻回一条，不用干等 30 秒。

用法：
    python body_probe.py [--base whale-a90c6c] [--broker broker.hivemq.com] [--timeout 8]
退出码 0 = 在线，1 = 没看到，2 = 连不上 broker。
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import body_mqtt  # noqa: E402
import body_config  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def main():
    # 连接参数（broker / 端口 / TLS / 账号）一律从 body_config 取：
    # 换 broker 只改 state/body-config.json，不用再改代码常量 —— 以前散在
    # body_mqtt / body_config / paths.json / 固件四处，漏一处就静默跳过身体。
    kw = body_config.link_kwargs()
    ap = argparse.ArgumentParser(description="探活：broker 上有没有板子的心跳")
    ap.add_argument("--base", default=kw["base"])
    ap.add_argument("--broker", default=kw["broker"])
    ap.add_argument("--timeout", type=float, default=8.0)
    args = ap.parse_args()

    kw["base"] = args.base
    kw["broker"] = args.broker
    link = body_mqtt.Link(**kw)
    try:
        link.connect(timeout=args.timeout)
    except RuntimeError as exc:
        print(json.dumps({"online": False, "why": str(exc)}))
        return 2
    try:
        st = link.wait_state(args.timeout)
        bad = link.health().get("state_bad", 0)
    finally:
        link.close()

    if not st:
        # 🔴 区分两种"没等到"：
        #   state_bad > 0 → 板子明明在发心跳，只是 JSON 坏了（固件 payload 太小）。
        #     这种情况别说"板子没开机"，否则一键启动会整段跳过身体，
        #     而真正的原因在固件里 —— 2026-10-28 就这么坑过一次。
        # 提到 finally 之前就取好了，这里直接用，别再读（close 之后无意义）。
        if bad:
            print(json.dumps({
                "online": False,
                "why": f"收到 {bad} 条 state 但 JSON 解析失败 —— "
                       "固件 main.c 的 payload[] 被 snprintf 截断了，"
                       "把它调大后重新烧录（不是板子没开机）",
            }, ensure_ascii=False))
            return 3
        print(json.dumps({"online": False, "why": "没等到心跳"}))
        return 1
    st["online"] = True
    print(json.dumps(st, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
