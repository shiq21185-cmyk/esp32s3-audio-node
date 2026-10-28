"""
udp_play.py —— 把一段 WAV 发给板子，从 MAX98357 放出来。

用法：
    python tools/udp_play.py <板子IP> <文件.wav>

例：
    python tools/udp_play.py 192.168.1.109 mic_mono.wav

会自动处理：
    - 采样率不是 16k 的，线性重采样到 16k
    - 单声道 / 立体声 都行（立体声取左声道，因为板子的麦也是存的左声道）
    - 按真实速度发（每 10ms 一包），板子那边缓冲 48KB，不会憋死

先 PING 一下确认板子在，再开始发。
"""

import argparse
import array
import math
import socket
import sys
import time
import wave

BOARD_PORT = 5005
LOCAL_PORT = 5007
AUDIO_MAGIC = 0xAA
TARGET_SR = 16000
PKT_FRAMES = 160          # 10ms @ 16k
PKT_BYTES = PKT_FRAMES * 4  # 立体声 16bit


def load_wav(path):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit(f"只支持 16bit WAV，这个文件是 {sw*8}bit")
    pcm = array.array("h")
    pcm.frombytes(raw[: (len(raw) // 2) * 2])
    if ch == 2:
        pcm = pcm[0::2]  # 取左声道
    elif ch != 1:
        raise SystemExit(f"不支持的声道数 {ch}")
    return list(pcm), sr


def resample(samples, src_sr, dst_sr):
    if src_sr == dst_sr:
        return samples
    ratio = dst_sr / src_sr
    out_len = int(len(samples) * ratio)
    out = []
    for i in range(out_len):
        pos = i / ratio
        i0 = int(pos)
        frac = pos - i0
        s0 = samples[i0] if i0 < len(samples) else 0
        s1 = samples[i0 + 1] if i0 + 1 < len(samples) else 0
        out.append(int(s0 + (s1 - s0) * frac))
    return out


def to_stereo_bytes(samples):
    buf = array.array("h", bytes(4 * len(samples)))
    for i, s in enumerate(samples):
        buf[2 * i] = s
        buf[2 * i + 1] = s
    return buf.tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board_ip")
    ap.add_argument("wav")
    ap.add_argument("--port", type=int, default=BOARD_PORT)
    ap.add_argument("--gain", type=float, default=1.0, help="音量倍数，默认 1.0")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", LOCAL_PORT))
    sock.settimeout(1.5)
    dest = (args.board_ip, args.port)

    print(f"[1/3] PING {dest[0]}:{dest[1]} …")
    sock.sendto(b"PING", dest)
    try:
        print("      板子回话:", sock.recvfrom(256)[0].decode("ascii", "replace"))
    except socket.timeout:
        print("      ❌ 没回应。板子 IP 对吗？在同一个网络里吗？")
        return 1

    print(f"[2/3] 读 {args.wav} …")
    samples, sr = load_wav(args.wav)
    dur_in = len(samples) / sr
    samples = resample(samples, sr, TARGET_SR)
    if args.gain != 1.0:
        samples = [max(-32768, min(32767, int(s * args.gain))) for s in samples]
    pcm = to_stereo_bytes(samples)
    dur = len(samples) / TARGET_SR
    print(f"      {sr} Hz → {TARGET_SR} Hz，{dur_in:.2f}s → {dur:.2f}s，"
          f"{len(pcm)/1024:.0f} KB")

    print(f"[3/3] 开始发送 … 应该马上能听到")
    t0 = time.time()
    sent = 0
    total_pkts = math.ceil(len(pcm) / PKT_BYTES)
    for i in range(0, len(pcm), PKT_BYTES):
        pkt = bytes([AUDIO_MAGIC]) + pcm[i : i + PKT_BYTES]
        sock.sendto(pkt, dest)
        sent += 1
        # 按真实时间放包，让板子的缓冲稳定在几十毫秒
        target = t0 + sent * (PKT_FRAMES / TARGET_SR)
        delay = target - time.time()
        if delay > 0:
            time.sleep(delay)

    # 补 0.4 秒静音，把尾巴推出去
    silence = bytes([AUDIO_MAGIC]) + bytes(PKT_BYTES)
    for _ in range(40):
        sock.sendto(silence, dest)
        time.sleep(0.01)

    elapsed = time.time() - t0
    print(f"      发完 {sent} 包，用时 {elapsed:.2f}s（音频时长 {dur:.2f}s）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
