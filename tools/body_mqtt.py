"""身体音频链路的 MQTT 版（不再依赖同一个局域网）。

D4 那套是 UDP 点对点：板子和 PC 必须在同一张网里。身体要带出去，
那条路就断了。现在两边都只做出站连接，经公共 broker 中转：

    PC --出站--> broker <--出站-- 板子

板子在 `firmware/d8-mqtt-audio`，主题约定（base = whale-1bb62897）：

    <base>/audio/up    板子 → PC   首字节 0xAC=3 帧 Opus 打包（0xAB=单帧，0xAA=裸 PCM）
    <base>/audio/down  PC → 板子   首字节由下行编码决定：0xAC=Opus 打包 / 0xAA=裸 PCM
    <base>/cmd         文本指令     "MIC 1" 开麦 / "MIC 0" 关麦 / "GATE 0|1" / "MODE 0|1"
    <base>/state       心跳 JSON   平时每 30 秒一条；状态一变（开麦/模式/门限）立刻补一条

线上是 **16k / 16bit / 单声道**（板子内部 I2S 仍是立体声，硬件要时钟；
它在收发的两头各做一次转换）。1920 字节 = 960 帧 = 60 ms。

中转是自己家的 EMQX Serverless（杭州），**TLS 8883 + 用户名密码**；
Serverless 不开明文端口，所以这里必须 use_tls=True。

麦克风默认是关的 —— 一上电就往公网推屋里声音不合适。
`mic(True)` / `mic(False)` 显式开关，录完必须关。
"""

from __future__ import annotations

import json
import queue
import ssl
import sys
import time
import array
import math
import uuid

import paho.mqtt.client as mqtt

SAMPLE_RATE = 16000
FRAME_BYTES = 2          # 16bit × 单声道
# ⚠️ 2026-10-28 实测：EMQX Serverless 卡的是「每秒消息条数」，不是带宽。
#    同一份数据，只改每包多长（延迟总量不变），PC→PC 走 broker 实测：
#      400ms/包 (2.5 包/s) → 1.02x 实时   ✅
#      200ms/包 (5.0 包/s) → 1.02x 实时   ✅
#      100ms/包 (10 包/s)  → 1.02x 实时   ✅
#       80ms/包 (12.5 包/s) → 0.87x 实时  ❌ 开始喂不上
#       60ms/包 (16.7 包/s) → 0.67x 实时  ❌ 严重跟不上（旧值，导致"后面卡"）
#    上限约 11 包/秒。60ms 时每秒欠 34%，开头靠预攒 900ms 撑着、
#    约 3 秒后缓冲见底 → 用户听到的正是"前面不卡后面卡"。
#    现在取 120ms（8.3 包/s），给 state 心跳和上行留足配额。
#    代价：单向多 120ms 延迟。对"她回话"的体感几乎无感，比卡顿好得多。
#    板子不关心包多大（dec_task 收到 0xAA 整包写 StreamBuffer），所以只改这里。
CHUNK_FRAMES = 1920
CHUNK_BYTES = CHUNK_FRAMES * FRAME_BYTES          # 3840
CHUNK_MS = CHUNK_FRAMES * 1000 // SAMPLE_RATE     # 120

# 包首字节 = 包类型。
# 2026-10-28：去掉 Opus，回裸 PCM。Opus 16kbps 对中文语音损伤太大
# （机械音 + 断续感），用户实机明确"压缩就卡，不压缩还行"。
# 流量代价：115MB/h（Opus 是 7.2MB/h），但 EMQX 额度够不够用先放一边，
# 先让她说话不卡才是第一位。
MAGIC_OPUS = 0xAB
MAGIC_PCM = 0xAA
MAGIC_BATCH = 0xAC
AUDIO_MAGIC = MAGIC_PCM     # 发出去用裸 PCM

DEFAULT_BASE = "whale-1bb62897"

# 自家的 EMQX Serverless（杭州）—— TLS 8883，带用户名密码。
# Serverless 只开 TLS 端口，没有明文 1883；证书是 DigiCert 公共 CA，系统信任即可。
DEFAULT_BROKER = "i1e78188.ala.cn-hangzhou.emqxsl.cn"
DEFAULT_PORT = 8883
# ⚠️ 账号密码**不写在这个文件里**（它可能被贴出去/进 git）。
# 由 body_config.credentials() 提供，优先级：
#   1) 环境变量 BODY_MQTT_USER / BODY_MQTT_PASS
#   2) state/body-config.json 的 mqttUser / mqttPass（本机文件，不进 git）
DEFAULT_USER = None
DEFAULT_PASSWORD = None
DEFAULT_TLS = True


def make_client() -> mqtt.Client:
    """paho 1.x / 2.x 都能用（2.x 换了回调签名，得显式选版本）。"""
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        return mqtt.Client()


def configure_client(client, user=DEFAULT_USER, password=DEFAULT_PASSWORD,
                     use_tls=DEFAULT_TLS):
    """给任何 paho Client 挂上这套 broker 需要的账号与 TLS。

    mqtt_audio.py / mqtt_probe.py 这些临时诊断工具也走这里，
    免得换 broker 时要改好几份常量。
    """
    if user is not None:
        client.username_pw_set(user, password or "")
    if use_tls:
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    return client


class Link:
    """一条连着板子的音频链路。用完记得 close()。"""

    def __init__(self, base=DEFAULT_BASE, broker=DEFAULT_BROKER,
                 port=DEFAULT_PORT, keepalive=30,
                 user=DEFAULT_USER, password=DEFAULT_PASSWORD,
                 use_tls=DEFAULT_TLS, client_id=None, down_codec=None):
        self.base = base
        self.broker = broker
        self.port = port
        self.keepalive = keepalive
        self.user = user
        self.password = password
        self.use_tls = use_tls
        self.client = make_client()
        # ⚠️ client_id 必须和板子不同：MQTT 同 id 后到者会把前者顶下线。
        self.client_id = client_id or ("pc-" + uuid.uuid4().hex[:12])
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.t_up = f"{base}/audio/up"
        self.t_down = f"{base}/audio/down"
        self.t_cmd = f"{base}/cmd"
        self.t_state = f"{base}/state"
        self._q: queue.Queue = queue.Queue()
        self._state = {}
        self._state_n = 0        # 收到过几条 state —— alive() 靠它判断"有没有新的一条"
        self._state_bad = 0      # 解析失败的 state 条数（>0 = 固件 payload 被截断了）
        self._connected = False
        self._last_error = ""
        self._decoder = None     # body_opus.Decoder，第一次收到 0xAB 才建
        self._enc = None         # body_opus.Encoder，第一次 send_audio 才建
        self._enc_failed = False
        self._rx_pkts = 0        # 收到过几包音频（不论编码）
        self._opus_pkts = 0
        self._pcm_pkts = 0
        self._bad_pkts = 0
        self._pend: list[bytes] = []   # 已编码但还没凑够一包发送的 Opus 帧
        # 下行编码：pcm = 裸 PCM（32KB/s，最省 CPU）；opus = 板子现场解码（2KB/s）
        # 传 None 表示"用全局默认 PCM"，由调用方（body_talk）从控制台配置里读。
        self.down_codec = "opus" if str(down_codec or "").lower() == "opus" else "pcm"
        self._pub_fail = 0       # publish 返回非成功值的次数
        self._disc = 0           # 掉线次数（on_disconnect 回调计数）
        self._pub_bytes = 0      # 已交付给 paho 的下行音频总字节

    # ---------------- 链路健康（给节拍器看） ----------------
    def health(self):
        """实时链路状况。**只读本地计数器**，不向板子发 PING（PING 会挤爆包率）。

        返回 dict：
          connected  当前是否连着 broker
          disc       累计掉线次数（网络断开/重连）
          pubfail    publish 返回失败的次数
          backlog    paho 内部还没写出去的报文条数
                     —— 持续变大 = 网络堵了（VPN / TLS 抖动），数据堆在本地，
                     网络一恢复就一次性冲出去，正好把链路「每秒包数」顶进丢包区。
          sentkb     已交付的下行音频字节数
          state_bad  解析失败的 state 条数
                     —— >0 几乎一定是固件的 payload[] 太小，JSON 被 snprintf
                     截断了。这时"板子不在线"是假象：板子在，是报文不完整。
        """
        try:
            backlog = len(self.client._out_messages)     # paho 内部待发队列
        except Exception:
            backlog = -1
        return {
            "connected": bool(self._connected),
            "disc": self._disc,
            "pubfail": self._pub_fail,
            "backlog": backlog,
            "sentkb": self._pub_bytes // 1024,
            "state_bad": self._state_bad,
        }

    def _get_decoder(self):
        """懒建解码器：不用 Opus 的场景（探活、切模式）不该为它付初始化代价。"""
        if self._decoder is None:
            from body_opus import Decoder
            self._decoder = Decoder()
        return self._decoder

    # ---------------- 回调 ----------------
    def _on_connect(self, client, userdata, flags, rc, *rest):
        # paho 2.x 给的是 ReasonCode，1.x 是 int —— 只有成功才算连上，
        # 否则密码错了也会误报"连上了"，排查时非常坑。
        ok = False
        try:
            ok = (int(rc) == 0)
        except Exception:
            ok = getattr(rc, "is_failure", None) is False or str(rc) == "Success"
        if not ok:
            self._last_error = str(rc)
            self._connected = False
            return
        self._connected = True
        client.subscribe(self.t_up, 0)
        client.subscribe(self.t_state, 0)

    def _on_disconnect(self, client, userdata, *rest):
        self._connected = False
        self._disc += 1

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        if topic == self.t_up:
            data = msg.payload
            if len(data) < 2:
                return
            self._rx_pkts += 1
            kind = data[0]
            if kind == MAGIC_BATCH:
                # 一包 N 帧，解完按顺序入队
                try:
                    from body_opus import split_batch
                    frames = split_batch(bytes(data[1:]))
                except Exception:
                    frames = []
                if not frames:
                    self._bad_pkts += 1
                    return
                dec = self._get_decoder()
                for fr in frames:
                    try:
                        pcm = dec.decode(fr)
                    except Exception:
                        pcm = b""
                    if pcm:
                        self._opus_pkts += 1
                        self._q.put(pcm)
                    else:
                        self._bad_pkts += 1
            elif kind == MAGIC_OPUS:
                # 一包一帧。解码在回调线程里做没关系：一帧 <1ms 的活，
                # 每秒才 17 次，不会拖住 paho 的收包循环。
                try:
                    pcm = self._get_decoder().decode(bytes(data[1:]))
                except Exception:
                    pcm = b""
                if pcm:
                    self._opus_pkts += 1
                    self._q.put(pcm)
                else:
                    self._bad_pkts += 1       # 坏包丢掉就好，Opus 有 PLC
            elif kind == MAGIC_PCM:
                self._pcm_pkts += 1
                self._q.put(bytes(data[1:]))
            return
        if topic == self.t_state:
            try:
                self._state = json.loads(msg.payload.decode("utf-8", "replace"))
                self._state_n += 1
            except Exception:
                # 🔴 2026-10-28 教训：这里原来是一个静默 pass，害死人。
                # 固件的 state 缓冲区曾经是 512 字节，字段加多以后 JSON 被
                # snprintf 截断在 511 字节（结尾停在 UTF-8 串中间），于是
                # 解析永远失败、self._state 永远是空 —— 但收发全在正常跑。
                # 表现是 PC 说"板子没回心跳/没开机"，而板子明明在线。
                # 现在把坏包计数起来，谁判"不在线"之前先看它。
                self._state_bad += 1
                if self._state_bad in (1, 10, 100, 1000):
                    tail = bytes(msg.payload[-24:])
                    print(f"[body_mqtt] ⚠️ state JSON 解析失败 "
                          f"(第 {self._state_bad} 次, {len(msg.payload)} 字节, "
                          f"结尾 {tail!r}) —— 多半是固件 payload[] 被截断了",
                          file=sys.stderr)

    # ---------------- 连接 ----------------
    def connect(self, timeout=10.0):
        self.client.reconnect_delay_set(min_delay=1, max_delay=10)
        configure_client(self.client, self.user, self.password, self.use_tls)
        self.client.connect_async(self.broker, self.port, self.keepalive)
        self.client.loop_start()
        end = time.time() + timeout
        while time.time() < end and not self._connected:
            time.sleep(0.05)
        if not self._connected:
            self.close()
            where = f"{self.broker}:{self.port}" + (" (TLS)" if self.use_tls else "")
            why = f" —— 服务器回：{self._last_error}" if self._last_error else ""
            raise RuntimeError(f"连不上 broker {where}{why}")
        # 板子空闲时 30 秒才发一次心跳。连上先敲一下，让它立刻回一条 state，
        # 这样探活只要几百毫秒（否则要等半分钟才能判断它在不在）。
        try:
            self.client.publish(self.t_cmd, "PING", 0)
        except Exception:
            pass
        return self

    def close(self):
        try:
            self.client.loop_stop()
        except Exception:
            pass
        try:
            self.client.disconnect()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---------------- 开关麦克风 ----------------
    def mic(self, on, wait=1.2):
        """开/关板子的麦克风。开了之后板子才开始往 <base>/audio/up 发包。

        返回是否在 wait 秒内看到 state 里 mic 真的变成想要的值
        （固件改完会立刻 publish_state("mic")，所以通常 0.1 秒内就能确认；
        等不到只说明这一条 state 没到，不代表开麦失败）。
        """
        self.client.publish(self.t_cmd, "MIC 1" if on else "MIC 0", qos=0)
        want = 1 if on else 0
        end = time.time() + wait
        while time.time() < end:
            if self._state.get("mic") == want:
                return True
            time.sleep(0.05)
        return False

    # ---------------- 收 ----------------
    def drain(self):
        """把缓冲里已经到的旧包全扔掉（比如上一轮她说话的回声）。"""
        n = 0
        while True:
            try:
                self._q.get_nowait()
                n += 1
            except queue.Empty:
                return n

    def recv(self, timeout=0.3):
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None

    @staticmethod
    def peak_of(pcm: bytes):
        """(峰值, 满量程样本占比) —— 用来区分真人声和 I2S 刚开流时的垃圾。"""
        arr = array.array("h")
        arr.frombytes(pcm[: len(pcm) - (len(pcm) % 2)])
        if not arr:
            return 0, 0.0
        pk = max(max(arr), -min(arr))
        bad = sum(1 for v in arr if v > 30000 or v < -30000)
        return pk, bad / len(arr)

    def _get_encoder(self):
        """懒建 Opus 编码器。只在下行选了 opus 时才建。

        起不来（没装 PyAV / 没有 libopus）就返回 None，调用方退回裸 PCM ——
        绝对不能因为省流量而让整句话消失。
        """
        if self._enc is not None:
            return self._enc
        if self._enc_failed:
            return None
        try:
            from body_opus import Encoder
            self._enc = Encoder()
            return self._enc
        except Exception as exc:
            self._enc_failed = True
            print(f"[body_mqtt] ⚠️ Opus 编码器起不来（{exc}）—— 下行退回裸 PCM",
                  file=sys.stderr)
            return None

    # ---------------- 发 ----------------
    def send_audio(self, payload: bytes):
        """往板子喇叭推一包音频（payload 是 16k 单声道裸 PCM，不带包头）。

        下行有两种走法，由 self.down_codec 决定：
          pcm  —— 直接发裸 PCM，首字节 0xAA。32KB/s，板子只搬运不解码。
          opus —— 编成 Opus 帧，凑 3 帧打成一个包，首字节 0xAC。约 2KB/s，
                  板子现场解码。这是 10-28 回退前的老路子，现在做成开关。

        ⚠️ 无论哪种，**包头都由这里加，sink 那边绝不能再加**
        （ts_to_board 的注释里记着这条：加两次会让 PCM 错开半个采样点=白噪音）。
        """
        if self.down_codec == "opus":
            enc = self._get_encoder()
            if enc is not None:
                try:
                    for fr in enc.encode(payload):
                        self._pend.append(fr)
                except Exception as exc:
                    # 编码器可能已被 drain 过（见 send_audio_flush 的注释）。
                    # 扔掉它重建一次再试，**不能直接把这一块丢了**——
                    # 丢了就是一段静音，而症状看不出来。
                    self._enc = None
                    retry = self._get_encoder()
                    if retry is not None:
                        try:
                            for fr in retry.encode(payload):
                                self._pend.append(fr)
                        except Exception as exc2:
                            print(f"[body_mqtt] ⚠️ Opus 编码失败（{exc} → 重建后仍 "
                                  f"失败 {exc2}），本块丢弃", file=sys.stderr)
                    else:
                        print(f"[body_mqtt] ⚠️ Opus 编码失败（{exc}），编码器重建失败，"
                              f"退回裸 PCM", file=sys.stderr)
                        self._send_pcm(payload)
                        return
                self._drain_batch()
                return
            # 落下来走 PCM

        self._send_pcm(payload)

    def _send_pcm(self, payload: bytes):
        """直接发裸 PCM（首字节 0xAA）。opus 走不通时的兜底。"""
        self._pub_bytes += len(payload) + 1
        try:
            info = self.client.publish(self.t_down, bytes([MAGIC_PCM]) + payload, qos=0)
        except Exception:
            self._pub_fail += 1
            return
        rc = info[0] if isinstance(info, tuple) else info
        try:
            if int(rc) != 0:
                self._pub_fail += 1
        except Exception:
            pass

    def _drain_batch(self):
        """把攒够 BATCH_FRAMES 帧的都打成一个包发出去。"""
        from body_opus import BATCH_FRAMES, pack_batch
        while len(self._pend) >= BATCH_FRAMES:
            group = self._pend[:BATCH_FRAMES]
            del self._pend[:BATCH_FRAMES]
            self._publish(bytes([MAGIC_BATCH]) + pack_batch(group))

    def _publish(self, data: bytes):
        """发一个下行包，顺带记账（健康检查要看）。"""
        self._pub_bytes += len(data)
        try:
            info = self.client.publish(self.t_down, data, qos=0)
        except Exception:
            self._pub_fail += 1
            return
        # paho 返回 (rc, msgid)；rc 不是 MQTT_ERR_SUCCESS 就是没进队列
        rc = info[0] if isinstance(info, tuple) else info
        try:
            if int(rc) != 0:
                self._pub_fail += 1
        except Exception:
            pass

    def send_audio_flush(self):
        """把编码器里剩下的零头挤出来发掉。

        PCM 模式什么都不用做（每个字节都已经发出去了）。
        Opus 模式必须做，否则每句话结尾会吞掉几十毫秒，听感是句尾「咔」一下。

        🔴 2026-10-28 血泪：PyAV 的 `cc.encode(None)` 会把 codec context 推进
        EOF（drain）状态，**之后同一个 context 再 encode 任何帧都抛
        `EOFError: [Errno 541478725] End of file`**。而编码器是缓存在
        `self._enc` 上跨轮复用的 ⇒ 只要 flush 过一次，这个进程后面所有句子
        全部编码失败、静音。
        真实现场：每轮开头 `beep()` 的结尾就调了一次 flush，于是「叮」之后
        她说的**每一个字都没到板子**，日志是「窗口内写 I2S 6480ms，静音 6480ms」。
        所以：**drain 完必须把编码器扔掉**，下一句重建（新建一次几毫秒，
        比整句话消失便宜得多）。
        """
        if self.down_codec != "opus":
            return
        enc = self._get_encoder()
        if enc is not None:
            try:
                for fr in enc.flush():
                    self._pend.append(fr)
            except Exception:
                pass
            finally:
                # 关键：flush 之后这个 encoder 已经作废，不能再用
                self._enc = None
        # 尾巴不足 3 帧也要发出去，不能留在 _pend 里等下一句
        if self._pend:
            from body_opus import pack_batch
            group = self._pend
            self._pend = []
            self._publish(bytes([MAGIC_BATCH]) + pack_batch(group))

    def beep(self, hz=880, ms=70, amp=2600):
        """从板子喇叭放一声很轻的"叮"，提示"轮到你说"。"""
        n = int(SAMPLE_RATE * ms / 1000)
        pcm = bytearray()
        for k in range(n):
            env = min(1.0, k / (SAMPLE_RATE * 0.005), (n - k) / (SAMPLE_RATE * 0.005))
            v = int(amp * env * math.sin(2 * math.pi * hz * k / SAMPLE_RATE))
            pcm += int(v).to_bytes(2, "little", signed=True)
        step = CHUNK_BYTES
        t0 = time.monotonic()
        idx = 0
        for off in range(0, len(pcm), step):
            chunk = bytes(pcm[off:off + step])
            if len(chunk) < step:
                chunk = chunk + b"\x00" * (step - len(chunk))
            self.send_audio(chunk)
            idx += 1
            target = t0 + idx * CHUNK_MS / 1000.0
            delay = target - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        self.send_audio_flush()

    @property
    def state(self):
        return dict(self._state)

    def sbuf_ms(self):
        """板子喇叭缓冲里还有多少毫秒音频（读最近一条 state，拿不到返回 None）。

        为什么需要它：发送端按死固定节拍（每 60ms 一块）实测会慢 13ms/块，
        缓冲被一路吃空，听感是"前面清楚、后面一卡一卡"。改成看水位发：
        水位低就立刻补发，把缓冲拉回来。32 字节 = 1 毫秒（16k 单声道）。
        """
        b = self._state.get("sbuf")
        if b is None:
            return None
        try:
            return int(b) / 32.0
        except (TypeError, ValueError):
            return None

    def snapshot(self, timeout=2.0):
        """敲一次 PING，等一条**新的** state 并返回它（拿不到返回 {}）。

        2026-10-27：每句话前后各取一次，用差值就能算出这句话在板子上
        「到底播干净了没有」：
          · spks 涨得多 → 板子被迫写静音（缓冲区空了 = 饿死 = 一卡一卡）
          · ddrop 涨了   → 缓冲区溢出丢帧（送太快 = 断字）
          · spkb 涨得好慢 → I2S 没跑起来
        只在句首句尾各敲一次，不会像狂打 PING 那样把链路包数顶进丢包区。
        """
        try:
            n0 = self._state_n
            self.client.publish(self.t_cmd, "PING", 0)
        except Exception:
            return {}
        end = time.time() + timeout
        while time.time() < end:
            if self._state_n > n0:
                return dict(self._state)
            if not self._connected:
                return {}
            time.sleep(0.02)
        # 超时兜底：板子偶尔一两秒不回 state（正忙着解码/播放），
        # 手里还留着上一条也不算瞎猜 —— 总比返回空、让调用方 KeyError 崩掉强。
        return dict(self._state) if self._state else {}

    def codec_stats(self):
        """收/发各用了什么编码 —— 排查"对方是不是还在发旧格式"最快的一眼。"""
        return {
            "rx": self._rx_pkts,
            "opus": self._opus_pkts,
            "pcm": self._pcm_pkts,
            "bad": self._bad_pkts,
        }

    def wait_state(self, timeout=6.0):
        """等板子的第一条 state —— 看到它就说明板子真的在线上（不是 PC 单方面）。"""
        end = time.time() + timeout
        while time.time() < end:
            if self._state:
                return self._state
            time.sleep(0.05)
        return {}

    def alive(self, timeout=3.0):
        """敲一下 PING，等一条**新的** state。True = 板子此刻真的在答话。

        为什么不靠心跳判断：板子空闲时 30 秒才发一条 state，
        想靠它做到"几秒内发现掉线"就得把心跳提到 5 秒 —— 那一个月又是 100MB。
        所以反着来：**由 PC 主动敲**，敲完立刻回，代价只有几十字节。
        拿到一条新的 state 才叫活；只是"之前收到过"不算（那可能是 29 秒前的旧账）。
        """
        try:
            n0 = self._state_n
            self.client.publish(self.t_cmd, "PING", 0)
        except Exception:
            return False
        end = time.time() + timeout
        while time.time() < end:
            if self._state_n > n0:
                return True
            if not self._connected:
                return False
            time.sleep(0.05)
        return False


def probe(base=None, broker=None, timeout=6.0):
    """板子在不在？返回它的 state（空字典 = 没看到）。

    ⚠️ 别用 Link() 的模块默认值凑合：`DEFAULT_USER/DEFAULT_PASSWORD` 都是
    None，`configure_client()` 就不会发账号密码 —— 连我们自己的 EMQX 会一直
    "Bad user name or password"。所以这里走 body_config.link_kwargs()，
    跟身体那条链路用同一份配置。
    """
    from body_config import link_kwargs
    kw = link_kwargs()
    if base:
        kw["base"] = base
    if broker:
        kw["broker"] = broker
    link = Link(**kw)
    try:
        link.connect()
        return link.wait_state(timeout)
    finally:
        link.close()
