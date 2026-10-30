#!/usr/bin/env python
"""让板子替她说一句话 —— 反方向：QQ 里的她 → 屋里的喇叭。

和 body_talk.py 的方向正好相反：

    body_talk.py  板子麦克风 ──> 她（耳朵）
    body_say.py   她 ──────────> 板子喇叭（嘴）

谁调它：桥接的 `POST /api/body/say`（由 `src/mcp-body.js` 的 `body_say` 工具触发）。
也就是说：**只有她自己决定要用身体说话时**，这里才会被拉起来。

用法（用 tools/asr 那个 venv 的 python，edge-tts 在那儿）：
    python tools/body_say.py "我在这边呢"
    python tools/body_say.py --text "我在这边呢" --volume 0.6
    python tools/body_say.py "..." --base whale-xxxx --broker xxx --no-wait

退出码：
    0  念完了
    1  参数有问题 / 板子不在线 / 合成失败
    2  板子在线但一句都没发出去（链路问题）

⚠️ 别在这里另起一套「连 broker + 念句子」的逻辑：音色、音量、
   下行编码（pcm/opus 热切换）、垫底节拍全在 body_talk.speak() 里，
   那些参数是拿十几轮实机卡顿换来的。这里只负责"把文字交过去"。
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import body_config   # noqa: E402  （控制台那份可调参数）
import body_mqtt     # noqa: E402  （公网中转那条链路）
import body_talk     # noqa: E402  （speak() / shorten() 都在这儿）

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def log(msg):
    print(msg, flush=True)


def main():
    ap = argparse.ArgumentParser(description="让板子替她说一句话")
    ap.add_argument("text", nargs="?", default="", help="要念的话")
    ap.add_argument("--text", dest="text_opt", default="", help="同位置参数（桥接用这个）")
    ap.add_argument("--base", default=None, help="主题前缀（缺省读控制台那份）")
    ap.add_argument("--broker", default=None, help="MQTT broker（缺省读控制台那份）")
    ap.add_argument("--volume", type=float, default=None,
                    help="播放增益 0~1.5（缺省读控制台的音量滑杆）")
    ap.add_argument("--timeout", type=float, default=10.0, help="连 broker 的超时秒数")
    ap.add_argument("--no-wait", action="store_true",
                    help="不等念完就返回（默认等，好让调用方拿到真实耗时）")
    a = ap.parse_args()

    text = (a.text_opt or a.text or "").strip()
    if not text:
        log("❌ 没给 --text，没话可念")
        return 1

    cfg = body_config.load(fresh=True)
    volume = a.volume if a.volume is not None else float(cfg.get("volume", 0.85))

    # 连接参数（broker/端口/TLS/账号/下行编码）全从同一份配置取。
    # 命令行显式给了 base/broker 才覆盖。
    kw = body_config.link_kwargs(fresh=True)
    if a.base:
        kw["base"] = a.base
    if a.broker:
        kw["broker"] = a.broker
        kw["use_tls"] = bool(cfg.get("brokerTls", kw.get("use_tls")))
        kw["port"] = int(cfg.get("brokerPort", kw.get("port")))
    log(f"📡 连 broker {kw['broker']}:{kw['port']}"
        f"{' (TLS)' if kw['use_tls'] else ''}，前缀 {kw['base']}，"
        f"下行 {kw['down_codec'].upper()}，音量 {volume:g}")

    t0 = time.time()
    try:
        link = body_mqtt.Link(**kw).connect(timeout=a.timeout)
    except RuntimeError as exc:
        log(f"❌ {exc}")
        return 1

    try:
        # 先确认板子在。不在就别白白合成一遍 —— 她那边等着的是一次工具调用，
        # 立刻说"板子没开机"比让它超时有用得多。
        st = link.wait_state(3.0)
        if not st:
            log("❌ 板子没回心跳（没上电 / 没连上 Wi-Fi / 主题前缀不对）"
                f" —— 前缀是 {kw['base']}")
            return 1
        log(f"✅ 板子在线（rssi {st.get('rssi')}，IP {st.get('ip')}，"
            f"模式 {'随身' if st.get('mode') == 1 else '室内'}）")

        before = link._rx_pkts  # 只是为了日志好读，不用它做判断
        body_talk.speak(text, link, volume=volume)
        if not a.no_wait:
            # speak() 是"按节拍推完就返回"，板子还在放缓冲里的最后几秒。
            # 等一下再关链路，否则最后那点数据可能还没发出去。
            left = link.sbuf_ms()
            if left:
                wait_s = min(12.0, left / 1000.0 + 0.8)
                log(f"   ⏳ 板子缓冲还剩 {left:.0f}ms，等它放完…")
                time.sleep(wait_s)
        h = link.health()
        log(f"✅ 念完了（{time.time() - t0:.1f}s，已发 {h['sentkb']}KB，"
            f"掉线 {h['disc']} 次，publish 失败 {h['pubfail']} 次）")
        if h["disc"] or h["pubfail"]:
            log("⚠️ 链路有抖动 —— 这句话可能断过，重连后再试一次也许更好")
            return 2
        return 0
    finally:
        try:
            link.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
