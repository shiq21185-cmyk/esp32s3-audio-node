"""走 MQTT 收发板子音频（不在同一个网也能用）。

    python mqtt_audio.py record --seconds 6 --out demo.wav
    python mqtt_audio.py play --file demo.wav
    python mqtt_audio.py tone --seconds 1.5          # 先放个测试音，确认喇叭通

连接参数走控制台配置（state/body-config.json），不用手填 broker/账号。

板子上行：<base>/audio/up    首字节 0xAC＝3 帧 Opus 打包，0xAB＝单帧 Opus，0xAA＝裸 PCM
板子下行：<base>/audio/down  首字节 0xAA＝16k 单声道 16bit 裸 PCM
（板子内部 I2S 仍是立体声，网络上只传一路，省一半带宽）
"""

from __future__ import annotations

import argparse
import array
import math
import struct
import sys
import time
import wave

import paho.mqtt.client as mqtt

import body_config
import body_mqtt

SAMPLE_RATE = 16000
FRAME_BYTES = 2          # 16bit × 单声道（线上是单声道，省一半带宽）
MAGIC = 0xAA            # 下行（电脑→板子）裸 PCM 的头
MAGIC_UP_BATCH = 0xAC   # 上行（板子→电脑）Opus 三帧打包的头 —— 板子现在就用这个
MAGIC_UP_OPUS = 0xAB    # 上行单帧 Opus（老格式，留着兼容）
CHUNK_BYTES = 1920       # 60 ms，和固件 config.h 里的 CHUNK_BYTES 保持一致
DEFAULT_BASE = body_mqtt.DEFAULT_BASE


def make_client() -> mqtt.Client:
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        return mqtt.Client()


def _wait_connected(state: dict, timeout: float = 8.0) -> bool:
    end = time.time() + timeout
    while not state.get("connected") and time.time() < end:
        time.sleep(0.05)
    return bool(state.get("connected"))


def _stats(pcm: bytes) -> tuple[int, float]:
    """返回 (峰值, RMS)，用 16bit 有符号样本算。"""
    n = len(pcm) // 2
    if n == 0:
        return 0, 0.0
    samples = array.array("h")
    samples.frombytes(pcm[: n * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    peak = 0
    acc = 0
    for s in samples:
        a = s if s >= 0 else -s
        if a > peak:
            peak = a
        acc += s * s
    return peak, math.sqrt(acc / n)


def _to_mono16k(pcm: bytes, channels: int, rate: int) -> bytes:
    """把手上的 PCM 变成 16k / 单声道 / 16bit（板子只认这个）。"""
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()

    if channels > 1:
        samples = samples[0::channels]   # 只取第一声道

    if rate != SAMPLE_RATE and len(samples):
        ratio = SAMPLE_RATE / float(rate)
        count = int(len(samples) * ratio)
        out = array.array("h", bytes(count * 2))
        for i in range(count):
            src = int(i / ratio)
            if src < len(samples):
                out[i] = samples[src]
        samples = out

    if sys.byteorder == "big":
        samples.byteswap()
    return samples.tobytes()


def record(args) -> int:
    state: dict = {}
    got = {"bytes": 0, "pkts": 0, "bad": 0}
    buf = bytearray()
    topic = f"{args.base}/audio/up"

    kw = body_config.link_kwargs()
    cli = body_mqtt.configure_client(
        make_client(), kw.get("user"), kw.get("password"), kw["use_tls"])

    def on_connect(c, u, flags, rc, *extra):
        state["connected"] = True
        c.subscribe(topic, qos=0)
        print(f"[订阅] {topic}")

    # 上行三种头都要认：0xAA 裸 PCM（很老）、0xAB 单帧 Opus、0xAC 多帧打包。
    # 2026-10-28 巡检发现的坑：这里原来只认 0xAA，而板子早改成 0xAC 上行，
    # 于是这个工具**一包都收不到**，还会把每包算成「坏包」—— 看起来像板子没开麦。
    dec = {"d": None}

    def on_message(c, u, msg):
        if msg.topic != topic:
            return
        payload = msg.payload
        if not payload:
            return
        kind = payload[0]
        if kind == MAGIC:
            buf.extend(payload[1:])
            got["pkts"] += 1
            got["bytes"] += len(payload) - 1
        elif kind in (MAGIC_UP_BATCH, MAGIC_UP_OPUS):
            import body_opus
            if dec["d"] is None:
                dec["d"] = body_opus.Decoder()
            try:
                frames = (body_opus.split_batch(bytes(payload[1:]))
                          if kind == MAGIC_UP_BATCH else [bytes(payload[1:])])
            except Exception:
                got["bad"] += 1
                return
            got["pkts"] += 1
            for fr in frames:
                pcm = dec["d"].decode(fr)
                if pcm:
                    buf.extend(pcm)
                    got["bytes"] += len(pcm)
                else:
                    got["bad"] += 1
        else:
            got["bad"] += 1

    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(args.broker, args.port, keepalive=30)
    cli.loop_start()

    if not _wait_connected(state):
        print("[失败] 连不上 broker")
        cli.loop_stop()
        return 1

    # 板子上的麦克风默认是关的（broker 是公共的，不该一直传屋里声音）。
    # 要录就显式开，录完必须关 —— 不然这个"开"会一直留着。
    cli.publish(f"{args.base}/cmd", "MIC 1", qos=0)
    time.sleep(0.3)
    print(f"[录 {args.seconds} 秒] 请对着板子说话…")
    start = time.time()
    while time.time() - start < args.seconds:
        time.sleep(0.1)
    cli.publish(f"{args.base}/cmd", "MIC 0", qos=0)
    time.sleep(0.2)
    cli.loop_stop()
    cli.disconnect()

    pcm = bytes(buf)
    peak, rms = _stats(pcm)
    dur = len(pcm) / (SAMPLE_RATE * FRAME_BYTES)
    print(f"[结果] {got['pkts']} 包 / {len(pcm)} 字节 / {dur:.2f} 秒 / 坏包 {got['bad']}")
    print(f"[电平] 峰值 {peak}  RMS {rms:.1f}")

    if args.out and pcm:
        with wave.open(args.out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(pcm)
        print(f"[已存] {args.out}")
    return 0


def play_pcm(pcm: bytes, args) -> int:
    state: dict = {}
    topic = f"{args.base}/audio/down"
    kw = body_config.link_kwargs()
    cli = body_mqtt.configure_client(
        make_client(), kw.get("user"), kw.get("password"), kw["use_tls"])

    def on_connect(c, u, flags, rc, *extra):
        state["connected"] = True

    cli.on_connect = on_connect
    cli.connect(args.broker, args.port, keepalive=30)
    cli.loop_start()
    if not _wait_connected(state):
        print("[失败] 连不上 broker")
        cli.loop_stop()
        return 1

    total = len(pcm)
    sent = 0
    t0 = time.time()
    while sent < total:
        block = pcm[sent:sent + CHUNK_BYTES]
        if len(block) < CHUNK_BYTES:
            block = block + bytes(CHUNK_BYTES - len(block))
        payload = bytes([MAGIC]) + block
        cli.publish(topic, payload, qos=0)
        sent += CHUNK_BYTES
        # 按实时速度推，否则板子缓冲会被瞬间灌满然后丢掉
        target = t0 + sent / (SAMPLE_RATE * FRAME_BYTES)
        ahead = target - time.time()
        if ahead > 0:
            time.sleep(ahead)

    # 尾巴多推一点静音，防止最后一段被 DMA 截掉
    for _ in range(6):
        cli.publish(topic, bytes([MAGIC]) + bytes(CHUNK_BYTES), qos=0)
        time.sleep(0.06)

    time.sleep(0.3)
    cli.loop_stop()
    cli.disconnect()
    print(f"[已推] {sent // CHUNK_BYTES} 包 / {total / (SAMPLE_RATE * FRAME_BYTES):.2f} 秒")
    return 0


def build_tone(seconds: float, freq: float = 1000.0, amp: float = 0.35) -> bytes:
    n = int(SAMPLE_RATE * seconds)
    out = array.array("h", bytes(n * 2))
    for i in range(n):
        out[i] = int(32767 * amp * math.sin(2 * math.pi * freq * i / SAMPLE_RATE))
    return out.tobytes()


def main() -> int:
    ap = argparse.ArgumentParser()
    # 同 mqtt_probe.py：一律走控制台配置，否则没有账号连不上 EMQX。
    kw = body_config.link_kwargs()
    ap.add_argument("--broker", default=kw["broker"])
    ap.add_argument("--port", type=int, default=kw["port"])
    ap.add_argument("--base", default=kw["base"])
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record")
    r.add_argument("--seconds", type=float, default=6.0)
    r.add_argument("--out", default=None)

    p = sub.add_parser("play")
    p.add_argument("--file", required=True)

    t = sub.add_parser("tone")
    t.add_argument("--seconds", type=float, default=1.5)
    t.add_argument("--freq", type=float, default=1000.0)
    t.add_argument("--amp", type=float, default=0.35)

    args = ap.parse_args()

    if args.cmd == "record":
        return record(args)
    if args.cmd == "tone":
        return play_pcm(build_tone(args.seconds, args.freq, args.amp), args)

    with wave.open(args.file, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        width = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width != 2:
        print(f"[失败] 只支持 16bit WAV，这个是 {width * 8}bit。先用 ffmpeg 转一下。")
        return 1
    pcm = _to_mono16k(raw, ch, rate)
    print(f"[读取] {args.file} {ch}ch {rate}Hz -> 16k 单声道 {len(pcm)} 字节")
    return play_pcm(pcm, args)


if __name__ == "__main__":
    raise SystemExit(main())
