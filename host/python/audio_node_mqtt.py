#!/usr/bin/env python
"""MQTT 客户端 + 命令行，配 firmware/audio-node-mqtt。

    python audio_node_mqtt.py --base node-1a2b3c4d ping
    python audio_node_mqtt.py --base node-1a2b3c4d record out.wav -t 6
    python audio_node_mqtt.py --base node-1a2b3c4d play out.wav
    python audio_node_mqtt.py --base node-1a2b3c4d tone -t 1.5

    # 需要账号密码 / TLS
    python audio_node_mqtt.py --broker my.broker:8883 --tls -u whale -p secret \
        --base node-1a2b3c4d ping

为什么是 MQTT 而不是 UDP：UDP 版本要求电脑和板子在同一张网里。MQTT 两边
都只做**出站连接**，板子挂在 4G 或别人的 Wi-Fi 上都行，只要 broker 公网可达。

依赖：paho-mqtt（`pip install paho-mqtt`）。音频处理只用标准库。
"""

from __future__ import annotations

import argparse
import array
import math
import os
import sys
import threading
import time
import wave

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover
    sys.exit("需要 paho-mqtt：pip install paho-mqtt")

# ---------------------------------------------------------------- 线上格式

SAMPLE_RATE = 16000
FRAME_BYTES = 2                 # 16 bit × 单声道
CHUNK_FRAMES = 1920             # 一个下行包装多少采样
CHUNK_BYTES = CHUNK_FRAMES * FRAME_BYTES       # 3840 = 120 ms
CHUNK_MS = CHUNK_FRAMES * 1000 // SAMPLE_RATE  # 120

MAGIC_PCM = 0xAA                # 首字节之后是裸 PCM（当前下行用的就是它）
MAGIC_OPUS = 0xAB               # 单帧裸 Opus（旧格式，板子还认）
MAGIC_BATCH = 0xAC              # [len_lo len_hi][Opus 帧]...  上行当前用这个

# 上行默认是 Opus 打包（省 16 倍流量）。解码要有 libopus：
#   pip install av
# 没有 av 也能用 —— 上行会退成裸 PCM(0xAA)，只是费流量。

DEFAULT_BASE = "node-00000000"


class Link:
    """一条到 broker 的连接，顺带管好四个话题。

    <base>/audio/up     板子 → 电脑，麦克风音频
    <base>/audio/down   电脑 → 板子，喇叭音频
    <base>/cmd          电脑 → 板子，文本指令
    <base>/state        板子 → 电脑，JSON 状态
    """

    def __init__(self, base: str, broker: str = "localhost", port: int = 1883,
                 use_tls: bool = False, user: str | None = None,
                 password: str | None = None, client_id: str | None = None):
        self.base = base
        self.broker = broker
        self.port = port
        self.use_tls = use_tls
        self._state: dict = {}
        self._state_at = 0.0
        self._state_n = 0          # 收到过几条 state —— alive() 靠它认"新到的那条"
        self._state_bad = 0        # 解析失败的 state 条数（>0 = 固件 payload 被截断）
        self._lock = threading.Lock()

        try:
            self._cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:                      # paho-mqtt 1.x
            self._cli = mqtt.Client()

        # client_id 必须和板子不同，否则同 id 后到者会把前者顶下线。
        self._cli.reconnect_delay_set(1, 5)
        if client_id:
            self._cli._client_id = client_id.encode()

        if user is not None:
            self._cli.username_pw_set(user, password)
        if use_tls:
            self._cli.tls_set()

        self._cli.on_connect = self._on_connect
        self._cli.on_message = self._on_message
        self._connected = threading.Event()
        self._last_error = ""         # 服务器拒连时回的原因（密码错、client_id 冲突…）
        self._audio_sink = None       # 设为 callable 就收音频

    # -- 连接 ------------------------------------------------------------

    def _on_connect(self, c, u, flags, rc, *extra):
        # ⚠️ 两个都必须订：只订 state 的话，麦克风音频（audio/up）根本收不到，
        #    record() 会永远返回空字节 —— 而 state 一切正常，看起来像"板子没开麦"。
        self._cli.subscribe(f"{self.base}/state", qos=0)
        self._cli.subscribe(f"{self.base}/audio/up", qos=0)
        # paho 2.x 的 rc 是 ReasonCode，密码错了也会进这里；不看 rc 就置"已连接"
        # 的写法会让错误密码表现成连上了。
        if int(getattr(rc, "value", rc)) != 0:
            self._last_error = f"{rc}"
            return
        self._connected.set()

    def _on_message(self, c, u, msg):
        topic = msg.topic
        if topic == f"{self.base}/state":
            import json
            try:
                with self._lock:
                    self._state = json.loads(msg.payload.decode("utf-8"))
                    self._state_at = time.time()
                    self._state_n += 1
            except Exception:
                # 这里原来是个静默 pass，害过人：固件那边的 state 缓冲区一旦装不下
                # 当前 JSON，snprintf 会静默截断（结尾停在半截 UTF-8 串里），于是
                # 解析永远失败、self._state 永远是空 —— 但收发其实全在正常跑。
                # 症状极具误导性：你说"板子没回心跳/没开机"，而板子明明在线。
                # 所以把坏包数起来，判"不在线"之前先看它。
                self._state_bad += 1
                if self._state_bad in (1, 10, 100, 1000):
                    tail = bytes(msg.payload[-24:])
                    print(f"[audio_node_mqtt] state JSON 解析失败 "
                          f"(第 {self._state_bad} 次, {len(msg.payload)} 字节, "
                          f"结尾 {tail!r}) —— 多半是固件 payload[] 被截断了",
                          file=sys.stderr)
            return
        if topic == f"{self.base}/audio/up":
            payload = msg.payload
            if not payload:
                return
            kind = payload[0]
            if kind == MAGIC_PCM:
                self._feed(payload[1:])
            elif kind == MAGIC_BATCH:
                for frame in split_batch(payload[1:]):
                    self._feed(decode_opus(frame))
            elif kind == MAGIC_OPUS:
                self._feed(decode_opus(payload[1:]))

    def _feed(self, pcm: bytes) -> None:
        sink = self._audio_sink
        if sink and pcm:
            sink(pcm)

    def connect(self, timeout: float = 8.0) -> "Link":
        try:
            self._cli.connect(self.broker, self.port, keepalive=30)
        except Exception as exc:
            raise RuntimeError(f"连不上 broker {self.broker}:{self.port} —— {exc}")
        self._cli.loop_start()
        if not self._connected.wait(timeout):
            self._cli.loop_stop()
            why = f" —— 服务器回：{self._last_error}" if self._last_error else ""
            raise RuntimeError(f"连不上 broker {self.broker}:{self.port}{why}")
        # 板子空闲时 30 秒才发一条心跳。连上先敲一下，让它立刻回一条 state，
        # 这样探活只要几百毫秒，不用干等半分钟。
        try:
            self.command("PING")
        except Exception:
            pass
        return self

    def close(self) -> None:
        try:
            self._cli.loop_stop()
            self._cli.disconnect()
        except Exception:
            pass

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- 收发 ------------------------------------------------------------

    def publish(self, suffix: str, payload) -> None:
        data = payload.encode() if isinstance(payload, str) else payload
        self._cli.publish(f"{self.base}/{suffix}", data, qos=0)

    def command(self, text: str) -> None:
        self.publish("cmd", text)

    def state(self) -> dict:
        with self._lock:
            return dict(self._state)

    def wait_state(self, timeout: float = 8.0) -> dict:
        """发一个 PING 然后等板子回 state。返回 {} = 没有回应。

        ⚠️ 判"板子在不在"只能靠这个（或等心跳），**不能靠"多久没收到音频包"**：
           室内模式下安静房间本来就是一个字节都不发，安静和掉线在数据流上
           长得一模一样。
        """
        self.command("PING")
        end = time.time() + timeout
        while time.time() < end:
            with self._lock:
                if self._state and time.time() - self._state_at < 3.0:
                    return dict(self._state)
            time.sleep(0.05)
        return {}

    def state_bad(self) -> int:
        """解析失败的 state 条数。判"板子没开机"之前先看这个：
        大于 0 说明 state 确实收到了，只是 JSON 被固件截断了。"""
        with self._lock:
            return self._state_bad

    def ping(self, timeout: float = 5.0) -> bool:
        return bool(self.wait_state(timeout))

    def alive(self, timeout: float = 3.0) -> bool:
        """敲一下 PING，等一条**新的** state。True = 板子此刻真的在答话。

        为什么不用「多久没收到包」判断掉线：室内模式下安静房间一个字节都不发，
        安静和掉线在数据流上长得一模一样，只能靠 PING。

        为什么不靠心跳：板子空闲时 30 秒才发一条 state，想靠它几秒内发现掉线
        就得把心跳提到 5 秒，一个月又是 100MB。反过来由电脑主动敲，敲完立刻回，
        代价几十字节。拿到一条新的才算活，"之前收到过"不算（那可能是 29 秒前的旧账）。
        """
        with self._lock:
            n0 = self._state_n
        try:
            self.command("PING")
        except Exception:
            return False
        end = time.time() + timeout
        while time.time() < end:
            with self._lock:
                if self._state_n > n0:
                    return True
            if not self._connected.is_set():
                return False
            time.sleep(0.05)
        return False

    def record(self, seconds: float, mic: bool = True) -> bytes:
        """录一段麦克风。板子上的麦克风默认是关的 —— 这里会显式开再关。"""
        buf = bytearray()
        lock = threading.Lock()

        def sink(pcm: bytes) -> None:
            with lock:
                buf += pcm

        self._audio_sink = sink
        try:
            if mic:
                self.command("MIC 1")
                time.sleep(0.3)
            time.sleep(seconds)
        finally:
            if mic:
                self.command("MIC 0")
            self._audio_sink = None
            time.sleep(0.2)
        with lock:
            return bytes(buf)

    def play(self, pcm: bytes) -> int:
        """按实时速度把 PCM 推给板子，返回推了多少包。

        按实时速度是关键：一次性灌进去会把板子缓冲瞬间撑满然后整段丢掉。
        """
        total = len(pcm)
        sent = 0
        t0 = time.time()
        while sent < total:
            block = pcm[sent:sent + CHUNK_BYTES]
            if len(block) < CHUNK_BYTES:
                block = block + bytes(CHUNK_BYTES - len(block))
            self.publish("audio/down", bytes([MAGIC_PCM]) + block)
            sent += CHUNK_BYTES
            ahead = (t0 + sent / (SAMPLE_RATE * FRAME_BYTES)) - time.time()
            if ahead > 0:
                time.sleep(ahead)
        # 尾巴补几包静音，免得最后一段卡在 DMA 里
        for _ in range(6):
            self.publish("audio/down", bytes([MAGIC_PCM]) + bytes(CHUNK_BYTES))
            time.sleep(CHUNK_MS / 1000)
        return sent // CHUNK_BYTES


# ---------------------------------------------------------------- 编解码

def split_batch(payload: bytes) -> list[bytes]:
    """[len_lo len_hi][frame]... → 若干 Opus 帧。残缺的尾巴丢掉。"""
    out = []
    off = 0
    n = len(payload)
    while off + 2 <= n:
        flen = payload[off] | (payload[off + 1] << 8)
        off += 2
        if flen <= 0 or off + flen > n:
            break
        out.append(payload[off:off + flen])
        off += flen
    return out


_opus_decoder = None
_opus_tried = False


def decode_opus(frame: bytes) -> bytes:
    """一个裸 Opus 包 → 16k 单声道 PCM。没有 PyAV 就返回空（上行会退成 PCM）。"""
    global _opus_decoder, _opus_tried
    if not _opus_tried:
        _opus_tried = True
        try:
            import av                                         # noqa: F401
            _opus_decoder = _make_opus_decoder()
        except Exception:
            print("[提示] 没装 PyAV，上行 Opus 解不了。pip install av "
                  "（或者让板子发裸 PCM）", file=sys.stderr)
    if _opus_decoder is None:
        return b""
    return _opus_decoder(frame)


def _make_opus_decoder():
    import av

    dc = av.CodecContext.create("libopus", "r")
    dc.format = "s16"
    dc.layout = "mono"
    dc.open()
    rs = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)

    def decode(frame: bytes) -> bytes:
        if not frame:
            return b""
        out = bytearray()
        try:
            frames = dc.decode(av.Packet(frame))
        except Exception:
            return b""
        for fr in frames:
            for rf in rs.resample(fr):
                out += rf.to_ndarray().tobytes()
        return bytes(out)

    return decode


# ---------------------------------------------------------------- WAV

def wav_read(path: str) -> bytes:
    with wave.open(path, "rb") as w:
        ch, rate, width = w.getnchannels(), w.getframerate(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width != 2:
        raise SystemExit(f"{path} 是 {width * 8}bit，只支持 16bit（先用 ffmpeg 转）")

    samples = array.array("h")
    samples.frombytes(raw[: len(raw) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    if ch > 1:
        samples = samples[0::ch]                      # 只取第一声道
    if rate != SAMPLE_RATE and len(samples):
        ratio = SAMPLE_RATE / float(rate)
        n = int(len(samples) * ratio)
        out = array.array("h", bytes(n * 2))
        for i in range(n):
            src = int(i / ratio)
            if src < len(samples):
                out[i] = samples[src]
        samples = out
    if sys.byteorder == "big":
        samples.byteswap()
    return samples.tobytes()


def wav_write(path: str, pcm: bytes) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


def level(pcm: bytes) -> tuple[int, float]:
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


def tone(seconds: float, freq: float = 1000.0, amp: float = 0.3) -> bytes:
    n = int(SAMPLE_RATE * seconds)
    out = array.array("h", bytes(n * 2))
    for i in range(n):
        out[i] = int(32767 * amp * math.sin(2 * math.pi * freq * i / SAMPLE_RATE))
    return out.tobytes()


# ---------------------------------------------------------------- CLI

def main() -> int:
    ap = argparse.ArgumentParser(
        description="MQTT 客户端，配 firmware/audio-node-mqtt")
    ap.add_argument("--base", default=os.environ.get("AUDIO_NODE_BASE", DEFAULT_BASE),
                    help="主题前缀，必须和固件 config.h 的 TOPIC_BASE 一样")
    ap.add_argument("--broker", default=os.environ.get("AUDIO_NODE_BROKER", "localhost"),
                    help="broker 主机名，可带 :端口")
    ap.add_argument("--port", type=int, default=None, help="端口，默认 1883 / TLS 时 8883")
    ap.add_argument("--tls", action="store_true", default=None)
    ap.add_argument("-u", "--user", default=os.environ.get("AUDIO_NODE_USER"))
    ap.add_argument("-p", "--password", default=os.environ.get("AUDIO_NODE_PASS"))

    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping", help="问板子在不在")
    sub.add_parser("state", help="打一整条状态 JSON")

    r = sub.add_parser("record", help="录麦克风到 WAV")
    r.add_argument("out")
    r.add_argument("-t", "--seconds", type=float, default=6.0)

    pl = sub.add_parser("play", help="把 WAV 从板子喇叭放出来")
    pl.add_argument("file")

    t = sub.add_parser("tone", help="放个测试音，先确认喇叭通")
    t.add_argument("-t", "--seconds", type=float, default=1.5)
    t.add_argument("--freq", type=float, default=1000.0)

    args = ap.parse_args()

    host, port = args.broker, args.port
    if ":" in host:
        host, _, tail = host.partition(":")
        if tail.isdigit():
            port = port or int(tail)
    use_tls = bool(args.tls) if args.tls is not None else False
    if port is None:
        port = 8883 if use_tls else 1883

    link = Link(args.base, host, port, use_tls, args.user, args.password)
    try:
        link.connect()
    except Exception as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        return 1

    try:
        if args.cmd in ("ping", "state"):
            st = link.wait_state()
            if not st:
                if link.state_bad():
                    print(f"[失败] state 收到了 {link.state_bad()} 条但 JSON 解析失败 "
                          "—— 固件 payload[] 被截断了，调大后重新烧录"
                          "（不是板子没开机）", file=sys.stderr)
                    return 3
                print("[失败] 8 秒内没收到 state —— 板子没开机 / 没连上 Wi-Fi / "
                      "主题前缀不一致 / broker 不对", file=sys.stderr)
                return 2
            if args.cmd == "ping":
                print(f"[在线] ip={st.get('ip', '?')} rssi={st.get('rssi', '?')} "
                      f"up={st.get('up', '?')}s heap={st.get('heap', '?')} "
                      f"mic={st.get('mic', '?')} mode={st.get('mode', '?')}")
            else:
                import json
                print(json.dumps(st, ensure_ascii=False, indent=2))
            return 0

        if args.cmd == "record":
            print(f"[录 {args.seconds:.1f} 秒] 请对着板子说话…")
            pcm = link.record(args.seconds)
            peak, rms = level(pcm)
            dur = len(pcm) / (SAMPLE_RATE * FRAME_BYTES)
            print(f"[结果] {dur:.2f} 秒 / {len(pcm)} 字节 / 峰值 {peak} / RMS {rms:.0f}")
            if not pcm:
                print("[提示] 一个包都没收到：检查 MIC 开了没、板子上有没有人声"
                      "（室内模式有人声门限，太安静就不发）", file=sys.stderr)
                return 3
            wav_write(args.out, pcm)
            print(f"[已存] {args.out}")
            return 0

        if args.cmd == "tone":
            pcm = tone(args.seconds, args.freq)
            n = link.play(pcm)
            print(f"[已推] {n} 包 / {args.seconds:.2f} 秒测音")
            return 0

        # play
        pcm = wav_read(args.file)
        print(f"[读取] {args.file} → 16k 单声道 {len(pcm)} 字节 "
              f"({len(pcm) / (SAMPLE_RATE * FRAME_BYTES):.2f} 秒)")
        n = link.play(pcm)
        print(f"[已推] {n} 包")
        return 0
    finally:
        link.close()


if __name__ == "__main__":
    raise SystemExit(main())
