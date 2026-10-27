"""
audio_node.py --- reference host-side client for esp32s3-audio-node.

Standard library only. Python 3.9+.

    from audio_node import AudioNode

    with AudioNode("192.168.1.109") as node:
        node.ping()                       # is it alive?
        node.record_wav("out.wav", 10)    # microphone -> file
        node.play_wav("reply.wav")        # file -> speaker

The board is deliberately dumb: it exposes one UDP port, four commands and
raw PCM. Anything smart (VAD, ASR, TTS, ducking, mixing) belongs on this
side. See docs/protocol.md.

Wire format, in one line: audio packets are `0xAA` + raw s16le stereo PCM
@ 16 kHz; every other packet is an ASCII command (PING / HELLO / STOP).
"""

from __future__ import annotations

import array
import math
import os
import socket
import threading
import time
import wave
from dataclasses import dataclass, field
from typing import Callable, Iterator, Optional

__all__ = [
    "AudioNode",
    "AudioFormat",
    "SAMPLE_RATE",
    "CHANNELS",
    "SAMPLE_WIDTH",
    "FRAME_BYTES",
    "DEFAULT_PORT",
    "AUDIO_MAGIC",
    "pcm_to_wav",
    "load_wav_as_pcm",
    "resample_pcm",
    "apply_gain",
]

# ---------------------------------------------------------------- constants

SAMPLE_RATE = 16000          # Hz
CHANNELS = 2                 # the board is stereo on the wire
SAMPLE_WIDTH = 2             # 16-bit
FRAME_BYTES = CHANNELS * SAMPLE_WIDTH   # 4
DEFAULT_PORT = 5005
AUDIO_MAGIC = 0xAA

_CHUNK_FRAMES = 320                       # 20 ms, what the board emits
_CHUNK_BYTES = _CHUNK_FRAMES * FRAME_BYTES
_PLAY_FRAMES = 160                        # 10 ms, what we send


@dataclass(frozen=True)
class AudioFormat:
    """Audio parameters, matching the firmware's config.h."""

    sample_rate: int = SAMPLE_RATE
    channels: int = CHANNELS
    sample_width: int = SAMPLE_WIDTH

    @property
    def frame_bytes(self) -> int:
        return self.channels * self.sample_width

    @property
    def bytes_per_second(self) -> int:
        return self.sample_rate * self.frame_bytes

    def duration_of(self, nbytes: int) -> float:
        return nbytes / self.bytes_per_second


# ---------------------------------------------------------------- pcm utils


def pcm_to_wav(pcm: bytes, path: str, fmt: AudioFormat = AudioFormat()) -> None:
    """Write raw PCM to a WAV file."""
    with wave.open(path, "wb") as w:
        w.setnchannels(fmt.channels)
        w.setsampwidth(fmt.sample_width)
        w.setframerate(fmt.sample_rate)
        w.writeframes(pcm)


def load_wav_as_pcm(path: str, fmt: AudioFormat = AudioFormat()) -> bytes:
    """
    Read a WAV file and return it as 16-bit PCM in `fmt`.

    Handles what people actually throw at a speaker: 8/16/24/32-bit,
    mono or stereo, any sample rate. Downmixes to mono first, then
    resamples, then duplicates to the board's stereo layout.
    """
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())

    samples = _decode_to_float(raw, sw)
    if ch > 1:
        samples = samples[::ch]          # left channel is enough
    if sr != fmt.sample_rate:
        samples = _resample_float(samples, sr, fmt.sample_rate)

    ints = array.array(
        "h", (max(-32768, min(32767, int(round(s * 32767)))) for s in samples)
    )
    return _to_stereo_bytes(ints)


def resample_pcm(pcm: bytes, src_rate: int, dst_rate: int,
                 fmt: AudioFormat = AudioFormat()) -> bytes:
    """Resample raw PCM between two rates (linear interpolation)."""
    if src_rate == dst_rate:
        return pcm
    ints = array.array("h")
    ints.frombytes(pcm[: (len(pcm) // 2) * 2])
    mono = ints[:: fmt.channels] if fmt.channels > 1 else list(ints)
    out = _resample_float([v / 32768.0 for v in mono], src_rate, dst_rate)
    ints2 = array.array("h", (max(-32768, min(32767, int(round(s * 32767)))) for s in out))
    return _to_stereo_bytes(ints2)


def apply_gain(pcm: bytes, gain: float) -> bytes:
    """Multiply PCM by `gain`, clipping. gain == 1.0 returns input untouched."""
    if gain == 1.0:
        return pcm
    ints = array.array("h")
    ints.frombytes(pcm[: (len(pcm) // 2) * 2])
    for i, v in enumerate(ints):
        ints[i] = max(-32768, min(32767, int(v * gain)))
    return ints.tobytes()


def _decode_to_float(raw: bytes, sample_width: int) -> list[float]:
    if sample_width == 1:            # 8-bit WAV is unsigned
        return [(b - 128) / 128.0 for b in raw]
    if sample_width == 2:
        a = array.array("h")
        a.frombytes(raw[: len(raw) // 2 * 2])
        return [v / 32768.0 for v in a]
    if sample_width == 3:
        out = []
        for i in range(0, len(raw) - 2, 3):
            v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if v & 0x800000:
                v -= 1 << 24
            out.append(v / 8388608.0)
        return out
    if sample_width == 4:
        a = array.array("i")
        a.frombytes(raw[: len(raw) // 4 * 4])
        return [v / 2147483648.0 for v in a]
    raise ValueError(f"unsupported sample width: {sample_width} bytes")


def _resample_float(samples: list[float], src: int, dst: int) -> list[float]:
    if src == dst or not samples:
        return samples
    ratio = dst / src
    n_out = int(len(samples) * ratio)
    out = [0.0] * n_out
    last = len(samples) - 1
    for i in range(n_out):
        pos = i / ratio
        i0 = int(pos)
        frac = pos - i0
        s0 = samples[i0] if i0 <= last else 0.0
        s1 = samples[i0 + 1] if i0 + 1 <= last else 0.0
        out[i] = s0 + (s1 - s0) * frac
    return out


def _to_stereo_bytes(ints: array.array) -> bytes:
    """Duplicate a mono stream into the board's stereo layout."""
    out = array.array("h", bytes(4 * len(ints)))
    out[0::2] = ints
    out[1::2] = ints
    return out.tobytes()


def _peak_and_rms(pcm: bytes) -> tuple[int, float]:
    ints = array.array("h")
    ints.frombytes(pcm[: (len(pcm) // 2) * 2])
    if not ints:
        return 0, 0.0
    left = ints[0::2] or ints
    peak = max(max(left), -min(left))
    rms = math.sqrt(sum(v * v for v in left) / len(left))
    return peak, rms


# ---------------------------------------------------------------- the node


@dataclass
class _State:
    streaming: bool = False
    packets: int = 0
    bytes: int = 0
    started_at: float = 0.0
    lock: threading.Lock = field(default_factory=threading.Lock)


class AudioNode:
    """
    One ESP32-S3 audio node on the local network.

    Not thread safe for concurrent use of the *same* socket: if you want a
    background reader and a foreground player at the same time, that is
    supported (reads and writes are independent), but two readers would
    steal packets from each other.
    """

    def __init__(self, host: str, port: int = DEFAULT_PORT,
                 local_port: int = 0, timeout: float = 2.0,
                 fmt: AudioFormat = AudioFormat()) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.fmt = fmt
        self._state = _State()

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("0.0.0.0", local_port))
        self._sock.settimeout(timeout)
        self.local_port = self._sock.getsockname()[1]

    # -- lifecycle ---------------------------------------------------

    def __enter__(self) -> "AudioNode":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        try:
            if self._state.streaming:
                self.stop()
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass

    @property
    def address(self) -> tuple[str, int]:
        return (self.host, self.port)

    # -- commands ----------------------------------------------------

    def _send(self, text: str) -> None:
        self._sock.sendto(text.encode("ascii"), self.address)

    def _recv(self, timeout: Optional[float] = None) -> Optional[bytes]:
        old = self._sock.gettimeout()
        if timeout is not None:
            self._sock.settimeout(timeout)
        try:
            data, _ = self._sock.recvfrom(4096)
            return data
        except socket.timeout:
            return None
        finally:
            if timeout is not None:
                self._sock.settimeout(old)

    def ping(self, retries: int = 2, timeout: float = 1.0) -> bool:
        """True if the board answers PONG."""
        for _ in range(retries):
            try:
                self._send("PING")
            except OSError:
                return False
            data = self._recv(timeout)
            if data and data.startswith(b"PONG"):
                return True
        return False

    def start(self, timeout: float = 2.0) -> bool:
        """Ask the board to start streaming its microphone to us."""
        self._send("HELLO")
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self._recv(max(0.05, deadline - time.time()))
            if data is None:
                continue
            if data.startswith(b"READY"):
                self._state.streaming = True
                self._state.packets = 0
                self._state.bytes = 0
                self._state.started_at = time.time()
                return True
        return False

    def stop(self) -> None:
        """Tell the board to stop streaming. Safe to call twice."""
        if not self._state.streaming:
            return
        self._send("STOP")
        self._state.streaming = False

    # -- upstream (microphone) ---------------------------------------

    def read_packet(self, timeout: Optional[float] = None) -> Optional[bytes]:
        """One 20 ms PCM packet, or None on timeout. Text replies are skipped."""
        data = self._recv(timeout if timeout is not None else self.timeout)
        while data is not None and data[:1] != bytes([AUDIO_MAGIC]):
            if data.startswith(b"PONG") or data.startswith(b"READY"):
                data = self._recv(0.01)
                continue
            # Unknown text packet; ignore rather than poison the audio stream.
            data = self._recv(0.01)
        if data is None:
            return None
        payload = data[1:]
        self._state.packets += 1
        self._state.bytes += len(payload)
        return payload

    def iter_packets(self, seconds: Optional[float] = None,
                     stop_event: Optional[threading.Event] = None,
                     idle_timeout: float = 2.0) -> Iterator[bytes]:
        """
        Yield PCM packets until `seconds` elapse, `stop_event` is set, or the
        board goes quiet for `idle_timeout` seconds.
        """
        t0 = time.time()
        last = t0
        while True:
            if stop_event is not None and stop_event.is_set():
                return
            if seconds is not None and time.time() - t0 >= seconds:
                return
            pkt = self.read_packet(min(0.2, idle_timeout))
            now = time.time()
            if pkt is None:
                if now - last > idle_timeout:
                    return
                continue
            last = now
            yield pkt

    def record_pcm(self, seconds: Optional[float] = None,
                   stop_event: Optional[threading.Event] = None,
                   progress: Optional[Callable[[float, int, float], None]] = None,
                   on_packet: Optional[Callable[[bytes], None]] = None) -> bytes:
        """
        Record from the microphone and return raw PCM (16 kHz stereo s16le).

        `progress(elapsed, peak, rms)` is called roughly 5x/second, which is
        enough to drive a VU meter without flooding a terminal.
        """
        chunks: list[bytes] = []
        if not self.start():
            raise ConnectionError(
                f"{self.host}:{self.port} did not answer HELLO "
                "(wrong IP? different Wi-Fi? firewall?)"
            )
        t0 = time.time()
        next_report = t0
        try:
            for pkt in self.iter_packets(seconds, stop_event):
                chunks.append(pkt)
                if on_packet is not None:
                    on_packet(pkt)
                now = time.time()
                if progress is not None and now >= next_report:
                    next_report = now + 0.2
                    peak, rms = _peak_and_rms(pkt)
                    progress(now - t0, peak, rms)
        finally:
            self.stop()
        return b"".join(chunks)

    def record_wav(self, path: str, seconds: Optional[float] = None,
                   stop_event: Optional[threading.Event] = None,
                   progress: Optional[Callable[[float, int, float], None]] = None
                   ) -> str:
        pcm = self.record_pcm(seconds, stop_event, progress)
        pcm_to_wav(pcm, path, self.fmt)
        return path

    # -- downstream (speaker) ----------------------------------------

    def play_pcm(self, pcm: bytes, gain: float = 1.0, realtime: bool = True,
                 packet_ms: int = 10, pad_ms: int = 400) -> int:
        """
        Send PCM to the speaker. Returns the number of packets sent.

        `realtime=True` paces packets at their true playback rate, which is
        what keeps the board's 1.5 s buffer from overflowing (dropped audio)
        or starving (stutter). Sending everything as fast as possible only
        works for clips shorter than the buffer.
        """
        pkt_bytes = self.fmt.sample_rate * packet_ms // 1000 * self.fmt.frame_bytes
        pkt_bytes -= pkt_bytes % self.fmt.frame_bytes
        pcm = apply_gain(pcm, gain)

        chunks = [pcm[i:i + pkt_bytes] for i in range(0, len(pcm), pkt_bytes)]
        if chunks and len(chunks[-1]) < pkt_bytes:      # pad the tail
            chunks[-1] += bytes(pkt_bytes - len(chunks[-1]))

        dest = self.address
        t0 = time.time()
        sent = 0
        for chunk in chunks:
            self._sock.sendto(bytes([AUDIO_MAGIC]) + chunk, dest)
            sent += 1
            if realtime:
                target = t0 + sent * (pkt_bytes / self.fmt.bytes_per_second)
                delay = target - time.time()
                if delay > 0:
                    time.sleep(delay)

        # Flush: without trailing silence the last 20 ms can stay stuck in
        # the DMA ring, because the speaker task keeps re-writing the same
        # half-second of buffer. Silence pushes the tail out.
        silence = bytes([AUDIO_MAGIC]) + bytes(pkt_bytes)
        n_pad = max(0, pad_ms // packet_ms)
        for i in range(n_pad):
            self._sock.sendto(silence, dest)
            if realtime:
                time.sleep(packet_ms / 1000.0)
        return sent

    def play_wav(self, path: str, gain: float = 1.0, realtime: bool = True) -> float:
        """Send a WAV file to the speaker. Returns its duration in seconds."""
        pcm = load_wav_as_pcm(path, self.fmt)
        self.play_pcm(pcm, gain=gain, realtime=realtime)
        return self.fmt.duration_of(len(pcm))

    # -- convenience -------------------------------------------------

    def loopback(self, seconds: float = 5.0) -> int:
        """
        Record while simultaneously echoing to the speaker (0.5 s delay).

        Handy as a smoke test, and as a party trick: it proves both
        directions work with one command.
        """
        if not self.ping():
            raise ConnectionError(f"no PONG from {self.host}:{self.port}")
        if not self.start():
            raise ConnectionError(f"{self.host}:{self.port} did not answer HELLO")

        stop = threading.Event()
        delay_packets = 25                      # 0.5 s at 20 ms per packet
        queue: list[bytes] = []

        def player() -> None:
            while not stop.is_set():
                if len(queue) > delay_packets:
                    pkt = queue.pop(0)
                    self._sock.sendto(bytes([AUDIO_MAGIC]) + pkt, self.address)
                else:
                    time.sleep(0.005)
            for pkt in queue:
                self._sock.sendto(bytes([AUDIO_MAGIC]) + pkt, self.address)

        t = threading.Thread(target=player, daemon=True)
        t.start()
        sent = 0
        try:
            for pkt in self.iter_packets(seconds):
                queue.append(pkt)
                sent += 1
        finally:
            stop.set()
            t.join(timeout=2.0)
            self.stop()
        return sent


# ---------------------------------------------------------------- CLI


def _cli() -> int:
    import argparse
    import sys

    ap = argparse.ArgumentParser(
        prog="audio_node",
        description="talk to an esp32s3-audio-node board",
    )
    ap.add_argument("--host", default=os.environ.get("AUDIO_NODE_HOST"),
                    help="board IP (or set AUDIO_NODE_HOST)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping", help="check the board is reachable")
    sub.add_parser("info", help="print the expected audio format")

    rec = sub.add_parser("record", help="record the microphone to a wav file")
    rec.add_argument("out", nargs="?", default="record.wav")
    rec.add_argument("-t", "--seconds", type=float, default=10.0)
    rec.add_argument("-q", "--quiet", action="store_true")

    play = sub.add_parser("play", help="play a wav file on the speaker")
    play.add_argument("wav")
    play.add_argument("-g", "--gain", type=float, default=1.0)

    loop = sub.add_parser("loopback", help="echo the microphone to the speaker")
    loop.add_argument("-t", "--seconds", type=float, default=5.0)

    args = ap.parse_args()

    if args.cmd == "info":
        print(f"sample rate : {SAMPLE_RATE} Hz")
        print(f"channels    : {CHANNELS} (stereo, mic data duplicated)")
        print(f"sample width: {SAMPLE_WIDTH * 8} bit")
        print(f"frame       : {FRAME_BYTES} bytes")
        print(f"bitrate     : {SAMPLE_RATE * FRAME_BYTES * 8 / 1000:.0f} kbit/s")
        print(f"packet      : {_CHUNK_BYTES} bytes = 20 ms")
        return 0

    if not args.host:
        ap.error("--host is required (or set AUDIO_NODE_HOST)")

    node = AudioNode(args.host, args.port)
    try:
        if args.cmd == "ping":
            ok = node.ping()
            print(f"{'PONG' if ok else 'no answer'} from {args.host}:{args.port}")
            return 0 if ok else 1

        if not node.ping():
            print(f"no PONG from {args.host}:{args.port} --- wrong IP, "
                  f"other network, or a firewall?", file=sys.stderr)
            return 1

        if args.cmd == "record":
            def show(elapsed: float, peak: int, rms: float) -> None:
                bar = "#" * min(40, int(rms / 40))
                print(f"\r  {elapsed:5.1f}s  peak={peak:5d}  rms={rms:7.1f}  {bar:<40}",
                      end="", flush=True)

            node.record_wav(args.out, args.seconds,
                            progress=None if args.quiet else show)
            print(f"\nwrote {args.out}")
            return 0

        if args.cmd == "play":
            dur = node.play_wav(args.wav, gain=args.gain)
            print(f"played {dur:.2f}s of audio")
            return 0

        if args.cmd == "loopback":
            n = node.loopback(args.seconds)
            print(f"looped {n} packets over {args.seconds:.1f}s")
            return 0
    finally:
        node.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
