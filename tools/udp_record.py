"""
udp_record.py —— 收板子发回来的麦克风声音，存成 WAV。

用法：
    python tools/udp_record.py <板子IP> [秒数] [输出文件]

例：
    python tools/udp_record.py 192.168.1.23 10 out.wav

原理：
    电脑先给板子发 "HELLO"（从本地 5006 端口发出），
    板子记住是谁问的，然后开始往回推 16kHz/16bit 立体声 PCM。
    本脚本把这些包按顺序拼成 WAV。

注意：
    Windows 防火墙可能会拦"接收 UDP"。如果一直收不到包，
    大概率是防火墙把 python.exe 的入站挡了。
"""

import argparse
import array
import socket
import sys
import time
import wave

BOARD_PORT = 5005
LOCAL_PORT = 5006
SAMPLE_RATE = 16000
CHANNELS = 2
SAMPLE_WIDTH = 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board_ip", help="板子 IP（串口日志里打印的那个）")
    ap.add_argument("seconds", nargs="?", type=float, default=10.0, help="录几秒，默认 10")
    ap.add_argument("outfile", nargs="?", default="mic_test.wav", help="输出 wav")
    ap.add_argument("--port", type=int, default=BOARD_PORT, help="板子 UDP 端口")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", LOCAL_PORT))
    sock.settimeout(1.0)

    dest = (args.board_ip, args.port)

    # 先 ping 一下，确认板子在
    print(f"[1/4] 给板子 {dest[0]}:{dest[1]} 发 PING …")
    sock.sendto(b"PING", dest)
    try:
        data, _ = sock.recvfrom(2048)
        print(f"      板子回话: {data.decode('ascii', 'replace')}")
    except socket.timeout:
        print("      ❌ 没回应。检查：板子 IP 对不对？板子和电脑在同一 Wi-Fi？防火墙？")
        return 1

    print("[2/4] 发 HELLO，让板子开始推流 …")
    sock.sendto(b"HELLO", dest)
    try:
        data, _ = sock.recvfrom(2048)
        txt = data.decode("ascii", "replace")
        if txt != "READY":
            print(f"      ⚠️ 回的不是 READY 而是 {txt!r}")
        else:
            print("      板子: READY")
    except socket.timeout:
        print("      ❌ HELLO 没得到响应")
        return 1

    print(f"[3/4] 开始录音 {args.seconds} 秒 … 现在对着板子说话")
    frames = []
    total_bytes = 0
    packets = 0
    t0 = time.time()
    last_print = 0.0

    while time.time() - t0 < args.seconds:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break
        if len(data) < 4:
            continue
        frames.append(data)
        total_bytes += len(data)
        packets += 1

        # 每 0.5 秒报一次当前音量，方便判断麦克风有没有在动
        now = time.time() - t0
        if now - last_print >= 0.5:
            last_print = now
            pcm = array.array("h")
            pcm.frombytes(data[: (len(data) // 2) * 2])
            # 取左声道
            left = pcm[0::2]
            if left:
                peak = max(abs(v) for v in left)
                rms = (sum(v * v for v in left) / len(left)) ** 0.5
                bar = "#" * min(40, int(rms / 40))
                print(f"      {now:5.1f}s  peak={peak:5d}  rms={rms:7.1f}  {bar}")

    print("[4/4] 停止推流，写文件 …")
    sock.sendto(b"STOP", dest)

    if not frames:
        print("      ❌ 一个包都没收到")
        return 1

    with wave.open(args.outfile, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(b"".join(frames))

    dur = total_bytes / (SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH)
    expected_packets = dur * 50  # 每包 20ms
    print()
    print(f"      收到 {packets} 包 / {total_bytes/1024:.0f} KB / {dur:.2f} 秒")
    print(f"      理论应有约 {expected_packets:.0f} 包，丢包率约 "
          f"{max(0.0, (1 - packets / expected_packets)) * 100:.1f}%")
    print(f"      文件: {args.outfile}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
