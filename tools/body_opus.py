#!/usr/bin/env python
"""裸 Opus 编解码（PC 这一侧）—— 用 PyAV 里的 libopus，不自己移植。

为什么需要它：裸 PCM 16k/16bit/单声道 = 32 KB/s = 115 MB/小时，
上下行一起一天就能把 EMQX 免费额度（1GB/月）吃穿。Opus 16kbps = 2 KB/s，
同样额度能讲一百多小时，出门用才不至于几个小时就没流量。

**线上是裸 Opus 包，不是 Ogg/WebM 容器** —— 每个 MQTT 包里正好一个
Opus 帧，首字节是 `AUDIO_MAGIC_OPUS`（0xAB）。板子那边 `esp_audio_codec`
的 `esp_opus_dec_decode` 吃的就是这种"帧边界已对齐"的裸包。

格式刻意和板子保持一致：
    16000 Hz / 单声道 / 16bit / 60ms 一帧（960 样本）/ 16000 bps CBR / VOIP

⚠️ libopus 解码**固定输出 48000 Hz**（ffmpeg 的 opus 解码器不理会
   `sample_rate` 设定），所以解码后必须重采样回 16k，否则时长差 3 倍、
   频率全错 —— 这个坑在 PyAV 上踩过一次，记在这里。
"""

from __future__ import annotations

import threading

import av

SAMPLE_RATE = 16000
FRAME_MS = 60
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000        # 960
FRAME_BYTES = FRAME_SAMPLES * 2                        # 1920（PCM 一边）
BITRATE = 16000                                        # bps，CBR
OPUS_PKT_BYTES = BITRATE * FRAME_MS // 1000 // 8       # 120（encode 后）
DECODED_RATE = 48000                                   # libopus 固定吐这个

# ---- 打包发送（2026-10-27 加）----
# 实测：这条链路按「每秒包数」节流，与包大小无关。
#   16.7 包/秒 → 只到 55~60%；8.3 包/秒 → 94%；5.6 包/秒 → 100%。
# 所以 3 帧（180 ms）合成一个 MQTT 包发。包格式：
#   [0xAC] 之后循环读 [len_lo len_hi][Opus 帧]，一直到包尾。
BATCH_FRAMES = 3


def pack_batch(frames: list[bytes]) -> bytes:
    """把若干完整 Opus 帧打成一个包（不带首字节 0xAC，由调用方加）。"""
    out = bytearray()
    for f in frames:
        out += len(f).to_bytes(2, "little") + f
    return bytes(out)


def split_batch(payload: bytes) -> list[bytes]:
    """把 0xAC 包的正文拆回若干 Opus 帧。残缺的尾巴直接丢。"""
    out: list[bytes] = []
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


class Encoder:
    """把 16k 单声道 PCM 一块块喂进去，吐出裸 Opus 包。

    每次 `encode(pcm)` 返回**若干个**完整包（凑不满一帧的零头留在内部，
    等下一块补上）—— 所以调用方可以给任意长度，不必自己对齐 960 样本。
    """

    def __init__(self, bitrate: int = BITRATE, complexity: int = 3):
        self._lock = threading.Lock()
        self._buf = bytearray()
        cc = av.CodecContext.create("libopus", "w")
        cc.sample_rate = SAMPLE_RATE
        cc.format = "s16"
        cc.layout = "mono"
        cc.bit_rate = bitrate
        # application=voip：优先"听得清"，不为音乐保真度浪费码率
        # frame_duration=60：和板子原来 60ms 一包的节奏完全一致
        cc.options = {"application": "voip", "frame_duration": str(FRAME_MS),
                      "vbr": "off", "compression_level": str(complexity)}
        cc.open()
        self.cc = cc

    def encode(self, pcm: bytes) -> list[bytes]:
        """输入任意长度的 16bit 单声道 PCM，返回这一批能凑出的完整 Opus 包。"""
        out: list[bytes] = []
        with self._lock:
            self._buf += pcm
            step = FRAME_SAMPLES * 2
            while len(self._buf) >= step:
                block = bytes(self._buf[:step])
                del self._buf[:step]
                frame = av.AudioFrame(format="s16", layout="mono",
                                      samples=FRAME_SAMPLES)
                frame.sample_rate = SAMPLE_RATE
                frame.planes[0].update(block)
                for pkt in self.cc.encode(frame):
                    out.append(bytes(pkt))
        return out

    def flush(self) -> list[bytes]:
        """把编码器里剩下的那点尾巴挤出来（一次流结束时调用）。"""
        out: list[bytes] = []
        with self._lock:
            if self._buf:
                pad = FRAME_SAMPLES * 2 - len(self._buf)
                block = bytes(self._buf) + b"\x00" * pad
                self._buf.clear()
                frame = av.AudioFrame(format="s16", layout="mono",
                                      samples=FRAME_SAMPLES)
                frame.sample_rate = SAMPLE_RATE
                frame.planes[0].update(block)
                for pkt in self.cc.encode(frame):
                    out.append(bytes(pkt))
            for pkt in self.cc.encode(None):
                out.append(bytes(pkt))
        return out


class Decoder:
    """把裸 Opus 包解回 16k 单声道 PCM。

    解码器是有状态的（Opus 内部有预测与重叠相加），**必须按到达顺序喂**，
    中间丢一两包不会炸，只是那一小段声音没了。
    """

    def __init__(self):
        self._lock = threading.Lock()
        dc = av.CodecContext.create("libopus", "r")
        dc.format = "s16"
        dc.layout = "mono"
        dc.open()
        self.dc = dc
        # libopus 固定 48k 输出，拉回 16k
        self.rs = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)

    def decode(self, pkt: bytes) -> bytes:
        """一个裸 Opus 包 → 16k 单声道 PCM（正常是 1920 字节）。坏包返回 b""。"""
        if not pkt:
            return b""
        out = bytearray()
        with self._lock:
            try:
                frames = self.dc.decode(av.Packet(pkt))
            except Exception:
                return b""      # 坏包直接丢，别卡住播放
            for fr in frames:
                for rf in self.rs.resample(fr):
                    out += rf.to_ndarray().tobytes()
        return bytes(out)

    def flush(self) -> bytes:
        """把重采样器里滞留的尾巴拉出来。"""
        out = bytearray()
        with self._lock:
            for rf in self.rs.resample(None):
                out += rf.to_ndarray().tobytes()
        return bytes(out)
