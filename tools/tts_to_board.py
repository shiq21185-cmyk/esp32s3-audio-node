#!/usr/bin/env python
"""把一句话用小鲸鱼的声音（edge-tts, zh-CN-XiaoyiNeural）念出来，从板子喇叭放出去。

链路：文字 -> edge-tts(mp3) -> ffmpeg(16k/16bit/立体声裸 PCM) -> UDP -> 板子

这就是 d4-duplex 固件约定的下行格式：
    UDP 发到板子的 5005 端口
    每个包第一个字节必须是 0xAA（AUDIO_MAGIC），后面跟裸 PCM
    PCM 必须是 16000Hz / 16bit / **立体声**（左右声道交错的 4 字节一帧）
    包长 1 + 4*N 字节，别超过 2048（固件收包缓冲 2048）

⚠️ 默认走**流式**：edge-tts 一吐出第一块音频就开始送板子，不等整段合成完。
   实测整段 2.7 秒的合成，流式下 0.5~0.7 秒就能听到第一个字 —— 对话里这点差别
   就是"她反应快不快"的全部。要听旧行为加 --no-stream。

用法（必须用桥接仓库那个 venv 的 python 跑，edge_tts / imageio_ffmpeg 都在那）：
    C:\\...\\qq-bridge-deploy\\tools\\asr\\.venv\\Scripts\\python.exe tools/tts_to_board.py "你好呀"
    ... tools/tts_to_board.py "你好呀" --ip 192.168.1.109 --voice zh-CN-XiaoyiNeural
"""
import argparse
import asyncio
import array
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

# --- 桥接仓库里的语音工具（她自己的声音，不另装一套） -------------------------
BRIDGE = Path(r"C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy")
VENV_PY = BRIDGE / "tools" / "asr" / ".venv" / "Scripts" / "python.exe"
TTS_PY = BRIDGE / "tools" / "asr" / "tts.py"

DEFAULT_VOICE = os.environ.get("TTS_VOICE") or "zh-CN-XiaoyiNeural"   # config.json 里她用的就是这个
EDGE_RATE = os.environ.get("TTS_RATE", "+0%")
EDGE_VOLUME = os.environ.get("TTS_VOLUME", "+0%")
EDGE_PITCH = os.environ.get("TTS_PITCH", "+0Hz")

DEFAULT_IP = "192.168.1.109"
UDP_PORT = 5005
AUDIO_MAGIC = 0xAA

SAMPLE_RATE = 16000
FRAME_BYTES = 4          # 16bit 立体声 = 4 字节/帧
CHUNK_FRAMES = 320       # 20ms
CHUNK_PAYLOAD = CHUNK_FRAMES * FRAME_BYTES   # 1280
CHUNK_MS = CHUNK_FRAMES * 1000 // SAMPLE_RATE  # 20


try:  # Windows 控制台默认 GBK，直接 print emoji / 中文会炸
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def log(msg):
    print(msg, flush=True)


def ffmpeg_exe():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return "ffmpeg"


def read_exact(stream, n):
    """从管道里读满 n 字节。

    ⚠️ 不能直接 `stream.read(n)` 当"一定读满"用：管道是流式的，
    ffmpeg 吐一块读一块，实测 166 次读里只有 24 次读满，
    大多数只给 768 字节。以前按块补零对齐，等于把人声切成碎片、
    中间塞满静音 —— 听感就是"沙沙的杂音 / 一顿一顿"。
    这里攒够整块再返回；真的到 EOF 才返回不足的一块（由调用方补零）。
    """
    buf = b""
    while len(buf) < n:
        piece = stream.read(n - len(buf))
        if not piece:
            break
        buf += piece
    return buf


# --- 老路：先整段合成到文件，再转码，再播 --------------------------------

def synth(text, voice, out_mp3):
    """调她自己那套 TTS（tts.py 里已经处理过文本清洗、重试、引擎选择）。"""
    cmd = [str(VENV_PY), str(TTS_PY), text, "--out", str(out_mp3), "--voice", voice]
    log("🎙  合成中…")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not out_mp3.exists():
        raise SystemExit("❌ TTS 失败：\n" +
                         (proc.stderr or proc.stdout or "(无输出)")[-1500:])
    tail = (proc.stdout or "").strip().splitlines()
    if tail:
        log("   " + tail[-1])
    return out_mp3.stat().st_size


def to_pcm(mp3, out_pcm):
    """mp3 -> 16k/16bit/立体声裸 PCM。ffmpeg 的 -ac 2 会把单声道复制成两路。"""
    cmd = [ffmpeg_exe(), "-y", "-loglevel", "error", "-i", str(mp3),
           "-f", "s16le", "-acodec", "pcm_s16le",
           "-ar", str(SAMPLE_RATE), "-ac", "2", str(out_pcm)]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not out_pcm.exists():
        raise SystemExit("❌ ffmpeg 转 PCM 失败：\n" + (proc.stderr or "")[-1200:])
    return out_pcm.stat().st_size


def play_pcm_bytes(data, ip, port, label="🔊"):
    """按实时节奏把 PCM 打进板子。节奏必须卡准：板子不排队，灌太快就丢。"""
    data = data[: len(data) - (len(data) % FRAME_BYTES)]
    total_frames = len(data) // FRAME_BYTES
    duration = total_frames / SAMPLE_RATE
    log(f"{label} 发给 {ip}:{port} —— {duration:.2f}s，{len(data)} 字节")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)

    t0 = time.monotonic()
    sent = 0
    for off in range(0, len(data), CHUNK_PAYLOAD):
        chunk = data[off: off + CHUNK_PAYLOAD]
        sock.sendto(bytes([AUDIO_MAGIC]) + chunk, (ip, port))
        sent += 1
        # 按"这一包应该在第几毫秒发出去"来对齐，而不是每包固定 sleep 20ms，
        # 否则 Python 的调度抖动会一路累积，最后整段音频越播越慢。
        target = t0 + (off + CHUNK_PAYLOAD) / (SAMPLE_RATE * FRAME_BYTES)
        delay = target - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    sock.close()
    log(f"✅ 发完 {sent} 包")


# --- 新路：边合成边播 ----------------------------------------------------

def _apply_gain(chunk, gain):
    """把 s16le 立体声的一块 PCM 乘上增益，削顶成 int16。

    只在控制台把音量从 1.0 调走的时候才会被调用 —— 64KB/s 的量，
    用 array 直接算比 numpy 起个依赖更省事。
    """
    n = len(chunk) // 2
    src = array.array("h")
    src.frombytes(chunk[: n * 2])
    out = array.array("h", [0]) * n
    for i, v in enumerate(src):
        s = int(v * gain)
        out[i] = 32767 if s > 32767 else (-32768 if s < -32768 else s)
    return out.tobytes()


def speak_streaming(text, ip=None, port=UDP_PORT, voice=DEFAULT_VOICE, timeout_s=30.0,
                    gain=1.0, sink=None, channels=2, chunk_bytes=None, chunk_ms=None,
                    prebuf_ms=800, lead_ms=0, level=None, health=None,
                    full_prebuf=False, full_prebuf_s=6.0):
    """边合成边送：先攒够 prebuf_ms 再开口，之后严格匀速。

    gain：播放增益（1.0=原始，0~1.5）。板子上没有硬件音量 —— MAX98357 的
    GAIN/SD 都悬空 —— 所以"小声一点"只能在 PCM 上乘。只在 gain≠1 时才做运算，
    免得白花 CPU。

    sink：把一包音频（含 0xAA 头）送出去的函数。给了就走它（MQTT 那条路），
    没给就自己开 UDP socket 发给 ip:port（d4 那套）。
    channels / chunk_bytes / chunk_ms：线上格式。UDP 那版是立体声 1280 字节 20ms；
    MQTT 那版是单声道 1920 字节 60ms。三个要一起改。

    返回 (音频毫秒数, 首包到发出用的毫秒数, 总耗时毫秒数)。

    prebuf_ms：开口前先攒多少毫秒音频（攒在本地队列里）。
    lead_ms：**开口时先一口气把这么多毫秒塞给板子**，让板子侧缓冲先有个垫底，
      之后才按 chunk_ms 匀速走。
      ⚠️ 血泪（2026-10-27）：早先没有 lead_ms，发送节奏和播放节奏**完全相等**，
        板子缓冲永远是 0 —— 网络一抖就断续，听起来"卡"。实测播放期间 sbuf
        有 25/60 次是 0。加上 lead 之后才有余量。
      但也不能给太多：早先版本"能超前就超前灌"整段会让板子爆音（突发灌=杂音）。
      折中：只垫开头一段（默认 0，由调用方给），之后仍然匀速。

    level：一个无参可调函数，返回**板子喇叭缓冲里还有多少毫秒**（拿不到返回 None）。
      给了就按水位发（水位低立刻补发），没给就按 chunk_ms 固定节拍发。
      2026-10-27：固定节拍实测慢 13ms/块，会把垫底吃光 → 听感"一卡一卡"。

    health：一个无参可调函数，返回链路状况 dict（`body_mqtt.Link.health()`）。
      2026-10-27 第四轮：用户实机"后面一卡一卡"，我本地怎么测都顺 ——
      因为**我这里不堵，他那里堵**。所以不再靠猜：发送时读**本地**链路计数器，
        · backlog 涨 = 报文堆在 paho 本地发不出去（VPN/TLS 抖动），
          网络一恢复就一次性冲出去，把链路包率顶进丢包区；
        · 掉线 = 重连期间板子会重放/断音。
      一旦发现堵，就把节拍降到 PACE_SLOW 慢慢喂，让 backlog 先排掉 ——
      道理和 TCP 拥塞控制一样：堵的时候猛灌只会更糟。

    full_prebuf：**整句合成完、全部编码好，再一次性匀速推给板子**。
      2026-10-27 第六轮，终于用固件自报的 I2S 字节数拿到了硬证据：
        · 板子自己的播放速率实测 16000 Hz 整点（100.0%/99.9%，空闲与开麦都测过）
        · 播放期间逐秒看，板子写出的静音占比 0%（缓冲始终 1900~2048ms，满的）
        · **但整段末尾必有一到两秒是纯静音** —— 队列空了，没有料了
      也就是说「前面清楚、后面一卡一卡」的真正原因是**送料节奏 vs 播放节奏对不齐，
      句尾必然把缓冲吃空**，而不是网络、不是编码、不是 I2S。
      所以干脆放弃"边合成边送"：先把整段料做齐（几秒），再一口气匀速推。
      代价：开口前多等 1~3 秒；换来全程缓冲满、一句到底不断。
      full_prebuf_s 是等料的封顶时间，超了就先开口（长句不至于等太久）。
    """
    import edge_tts

    if chunk_bytes is None:
        chunk_bytes = CHUNK_PAYLOAD
    if chunk_ms is None:
        chunk_ms = chunk_bytes // (2 * channels) * 1000 // SAMPLE_RATE

    cmd = [ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
           "-f", "mp3", "-i", "pipe:0",
           "-f", "s16le", "-acodec", "pcm_s16le",
           "-ar", str(SAMPLE_RATE), "-ac", str(channels), "pipe:1"]
    ff = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, bufsize=0)

    boom = {}

    def feed():
        """一个线程专门喂 mp3 给 ffmpeg；主线程同时在读 ffmpeg 的 PCM 输出，
        两个方向都有人在动，管道才不会互相堵死。"""
        async def once():
            """合成一次。返回写入的 mp3 字节数。整段失败会抛异常。"""
            comm = edge_tts.Communicate(text, voice, rate=EDGE_RATE,
                                        volume=EDGE_VOLUME, pitch=EDGE_PITCH)
            wrote = 0
            async for chunk in comm.stream():
                if chunk.get("type") == "audio" and chunk.get("data"):
                    ff.stdin.write(chunk["data"])
                    ff.stdin.flush()
                    wrote += len(chunk["data"])
            return wrote

        async def run():
            # 🔴 2026-10-28：edge-tts 会**偶发**抛 NoAudioReceived
            # （WebSocket 连上了但服务端一个音频帧都不回）。
            # 实测同一句话连跑 3 次：成功、成功、失败 —— 不是参数问题、不是
            # 文本问题、不是网络断，纯粹是它自己抽风。
            # 以前一次失败 = 整句话消失，用户听感就是"她突然不说话/漏答一轮"。
            # 所以：**一个字节都没收到**就重试（最多 4 次，间隔递增）；
            # 已经收到过音频的失败不重试，否则会把前半句重复念一遍。
            last = None
            for attempt in range(4):
                try:
                    wrote = await once()
                    if wrote:
                        return
                    last = RuntimeError("edge-tts 一个音频字节都没返回")
                except Exception as exc:
                    last = exc
                await asyncio.sleep(0.35 * (attempt + 1))
            raise last

        try:
            asyncio.run(run())
        except Exception as exc:            # 网络抖动 / 文本被服务端拒
            boom["err"] = exc
        finally:
            try:
                ff.stdin.close()
            except Exception:
                pass

    th = threading.Thread(target=feed, daemon=True)
    th.start()

    sock = None
    if sink is None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)

        def ship(payload):
            sock.sendto(bytes([AUDIO_MAGIC]) + payload, (ip, port))
    else:
        # ⚠️ 这里**不能**再加 0xAA：sink 自己负责加头（body_mqtt.Link.send_audio 会加）。
        # 两头都加过的后果：MQTT 上是 1922 字节，板子剥掉第一个 0xAA 之后，
        # 剩下的 PCM 整体错开一个字节（半个采样点）—— 听起来就是一路白噪音。
        # 找了两天的"杂音"根因就是这一行。
        def ship(payload):
            sink(payload)

    import collections

    started = time.monotonic()
    q = collections.deque()
    qlock = threading.Lock()
    eof = threading.Event()
    qev = threading.Event()

    def reader():
        """一个线程只管把 PCM 从管道里读出来攒进队列，主线程只管按节拍发。"""
        while True:
            chunk = read_exact(ff.stdout, chunk_bytes)
            if not chunk:
                break
            if len(chunk) < chunk_bytes:
                chunk = chunk + b"\x00" * (chunk_bytes - len(chunk))
            if gain != 1.0:
                chunk = _apply_gain(chunk, gain)
            with qlock:
                q.append(chunk)
            qev.set()
        eof.set()
        qev.set()
    threading.Thread(target=reader, daemon=True).start()

    # ① 先攒够 prebuf_ms 再开口 —— 手里有存货，后面才有资格谈"匀速"。
    have = 0
    while not eof.is_set():
        with qlock:
            have = len(q)
        if have * chunk_ms >= prebuf_ms:
            break
        qev.wait(0.02)
        qev.clear()

    if full_prebuf:
        # 2026-10-28 第一版：不再"等整句合成完"。
        # 旧逻辑是攒满 full_prebuf_s（8 秒）才开口 —— 攒料期间板子在播静音，
        # 听感就是"开口前等很久 + 中间有停顿间隔"。
        # 新逻辑：只等 prebuf_ms（默认 2500ms）就开口，之后边合成边送。
        # 这样攒料期短、静音少，而且 edge-tts 分段到达的特性被利用起来：
        # 板子缓冲里始终有存货，不需要等整段。
        with qlock:
            have = len(q)
        log(f"   ⏳ 预攒 {have} 块 = {have * chunk_ms}ms，边合成边送")

    # ② 之后严格每 chunk_ms 发一包，和"从文件匀速推"完全同一种节奏。
    #    为什么不能"能发多快就发多快"：实测同一份 PCM，
    #      从文件匀速推 = 正常人声；按漏斗超前灌 = 全杂音。
    #    板子那边缓冲够也不会救 —— 突发本身就是噪声源（大概率是 TCP/MQTT
    #    下行突发 + 板子 I2S 写入时序互相踩）。所以：宁可等，不要冲。
    t0 = time.monotonic()
    first_ms = int((t0 - started) * 1000)
    idx = 0
    gaps = 0
    log(f"🔊 开始放（预攒 {have * chunk_ms}ms 音频，等待 {first_ms}ms）")

    # ①' 先垫一段：把 lead_ms 的音频一口气塞给板子，让它的缓冲先有存货。
    #     这段时间不占发送节拍（板子本来就在播，我们只是超前送）。
    lead_n = max(0, int(lead_ms // chunk_ms)) if lead_ms > 0 else 0
    while idx < lead_n:
        with qlock:
            chunk = q.popleft() if q else None
        if chunk is None:
            if eof.is_set():
                break
            qev.wait(0.03)
            qev.clear()
            continue
        ship(chunk)
        idx += 1
    if lead_n and idx:
        log(f"   ↳ 垫了 {idx * chunk_ms}ms 给板子打底")

    # ②' 之后按「94% 实时率 + 水位上限」发。
    #
    # 2026-10-27 第三轮血泪（前两版都没治到根）：
    #   第一版：t_next = now + 60ms，误差逐块累积，实测 73ms/块。
    #   第二版：绝对时间轴（anchor + k*60ms），误差不累积了 —— **但仍然偏慢**。
    #     因为主循环每块自己就要花 ~4ms（取队加锁 + Opus 编码 + TLS 发布），
    #     而一轮只发一块，落后了就再也追不回来。
    #     实测 323 块用了 20625ms = 每块 63.9ms，比播放慢 6%。
    #     19 秒长句累计欠 1.2 秒，把 900ms 垫底吃空 → "后面一卡一卡"。
    #
    # 这一版改两件事：
    #   · 节拍按 94% 实时走（每块 56.4ms），留 6% 余量，天然追得回来；
    #   · 用**自己记账**的水位模型当闸门：水位 = 垫底 + 已送出 − 已流逝。
    #     涨到 LEVEL_HIGH 就歇一下（别顶满板子 2048ms 缓冲 → 溢出丢帧又是杂音），
    #     落后了就立刻补。这个闸门自己会锁在"正好等于播放速度"上。
    #   ⚠️ 不能用 link.sbuf_ms() 当闸门：它是最后一次收到的 state **静态快照**，
    #      播放期间不 PING 就不更新；而狂打 PING 会把链路包率顶进丢包区
    #      （实测带 PING 送达 82%、不带 100%）。
    # 2026-10-27 第六轮：整句预攒时用**严格 1.0 倍**节拍。
    #   以前用 0.94（为了追回"每块自己花 4ms"的累积误差），那是因为节拍是
    #   相对 time 递推的。现在节拍是**绝对时间轴**（due = t_clock + k*chunk_ms*pace），
    #   4ms 的循环开销既不会累积、也不需要提前 6% 去弥补。
    #   而板子缓冲只有 2048ms，0.94 会让水位一路涨到顶 → 溢出丢帧（ddrop）又是杂音。
    #   实测板子播放速率 = 16000Hz 整点（100.0%/99.9%），所以我们按 1.0 送、
    #   水位就锁在 lead_ms 上，全程不见底也不顶满 —— 这才是"一句到底不断"。
    #
    # 2026-10-27 第九轮（1.0 是错的，实测证明）：
    #   用户实测日志：`发完 286 包（17160ms 音频，总耗时 18469ms）` = **93.3% 实时**。
    #   也就是说 pace=1.0 只是"目标"，实际每块比 due 晚 4.5ms（Windows sleep 粒度
    #   15.6ms + ship 自己花 4ms），跑满 17 秒就欠 1.1 秒。
    #   改成 0.94 主动留 6% 余量 —— 每块提前一点点，欠账由"追回来"变成"永远不会欠"。
    #   配合板子缓冲从 2 秒加到 8 秒（firmware main/config.h SPK_BUFFER_BYTES），
    #   水位稳定在高位，用户听不出任何速度差。
    PACE = 0.94
    PACE_SLOW = 1.35       # 链路堵的时候放慢（每块要花 chunk_ms*1.35 才发下一块）
    # 水位上下限跟着缓冲走：现在板子 8192ms，高位放到 5000（够 5 秒存货），
    # 低位 1200（低于这个立刻全速补，别等）。
    LEVEL_HIGH = 5000.0
    LEVEL_LOW = 1200.0
    # 注意：水位高**不增加开口延迟** —— 板子收到第一块就开始播，水位只是"前方还有多少存货"，
    # 是抗卡余量，不是排队等待。

    t_clock = time.monotonic()
    lead = float(idx * chunk_ms)     # 垫底已经送出去多少毫秒音频
    k = 0
    level_low = lead                 # 全程最低水位，最后打出来看有没有见底
    backlog_max = 0                  # 全程最大积压（paho 本地待发报文条数）
    disc0 = None                     # 开始时的掉线次数
    congested = 0                    # 判定为"堵"的次数
    last_h = {}                      # 最后一次读到的健康数据
    ship_us = []                     # 每块 ship() 自己花了多少微秒（编码+发布）
    sleep_us = 0.0                   # 节拍等待累计微秒
    starve_us = 0.0                  # 等 TTS 合成（队列空）累计微秒
    hi_us = 0.0                      # 水位偏高主动让路累计微秒
    hi_hits = 0                      # 让路次数
    qmin_ms = 1 << 30                # 全程队列最低存货（毫秒）

    while True:
        now = time.monotonic()
        level = lead + k * chunk_ms - (now - t_clock) * 1000.0
        if level < level_low:
            level_low = level

        # ---- 链路健康：只看本地计数器（不发 PING，PING 会挤爆包率） ----
        pace = PACE
        if health is not None:
            try:
                h = health()
                last_h = h
                if disc0 is None:
                    disc0 = h.get("disc", 0)
                bl = h.get("backlog", -1)
                if bl > backlog_max:
                    backlog_max = bl
                # paho 本地积压说明链路已经发不出去了。整句预攒模式下料全在手边，
                # 一旦积压，网络恢复时会一次性冲出好几个包 —— 板子 2048ms 缓冲
                # 一次冲 5 包（1200ms）就顶满 → 溢出丢帧 → 断字。
                # 所以阈值压到 2 包（480ms），一有苗头就放慢，宁可晚一点也别爆。
                if bl >= (2 if full_prebuf else 6):
                    congested += 1
                    pace = PACE_SLOW
            except Exception:
                pass

        if level > LEVEL_HIGH:
            t_hi = time.monotonic()
            time.sleep(0.01)              # 存货够了：等它播掉一点再送
            hi_us += (time.monotonic() - t_hi) * 1e6
            hi_hits += 1
            continue

        # 只在"水位健康"时才按节拍等；一旦低于 LEVEL_LOW，就跳过下面的 due 检查、
        # 立刻补发 —— 否则 6% 的追赶速度救不回一次 100ms 的网络卡顿
        # （追 1 秒要 17 秒，长句早就听出断续了）。
        if level >= LEVEL_LOW:
            due = t_clock + (k * chunk_ms * pace) / 1000.0
            if now < due:
                t_sl = time.monotonic()
                time.sleep(min(due - now, 0.02))
                sleep_us += (time.monotonic() - t_sl) * 1e6
                continue                  # 睡醒重算，别带偏

        with qlock:
            chunk = q.popleft() if q else None
            qleft = len(q) * chunk_ms
        if chunk is None:
            if eof.is_set():
                break
            # 队列空 = TTS 还没合成出来。这**不是**网络问题，是供给跟不上。
            gaps += 1
            t_w = time.monotonic()
            qev.wait(0.03)
            qev.clear()
            starve_us += (time.monotonic() - t_w) * 1e6
            continue
        if qleft < qmin_ms:
            qmin_ms = qleft

        t_ship = time.monotonic()
        ship(chunk)
        ship_us.append((time.monotonic() - t_ship) * 1e6)
        idx += 1
        k += 1

        if idx < 2 and time.monotonic() - started > timeout_s:
            break

    if sock is not None:
        sock.close()
    try:
        ff.wait(timeout=3)
    except subprocess.TimeoutExpired:
        ff.kill()

    if idx == 0:
        why = boom.get("err")
        raise SystemExit("❌ 流式 TTS 没出音：" + (f"{type(why).__name__}: {why}" if why else "edge-tts 返回空"))

    audio_ms = idx * chunk_ms
    total_ms = int((time.monotonic() - started) * 1000)
    log(f"✅ 发完 {idx} 包（{audio_ms}ms 音频，总耗时 {total_ms}ms，缺料 {gaps} 次，"
        f"最低水位 {level_low:.0f}ms）")
    if health is not None and last_h:
        discn = last_h.get("disc", 0) - (disc0 or 0)
        note = ""
        if backlog_max >= 6:
            note = "  ⚠️ 链路堵过"
        if discn:
            note += f"  ⚠️ 掉线 {discn} 次"
        log(f"   链路：最大积压 {backlog_max} 包 ｜ 慢速节拍 {congested} 次 ｜ "
            f"掉线 {discn} 次 ｜ publish 失败 {last_h.get('pubfail', 0)}{note}")
    if ship_us:
        s = sorted(ship_us)
        p50 = s[len(s) // 2] / 1000.0
        p90 = s[min(len(s) - 1, int(len(s) * 0.9))] / 1000.0
        avg = sum(ship_us) / len(ship_us) / 1000.0
        tot_wall = (time.monotonic() - t_clock) * 1e6
        log(f"   耗时分解（每块）：ship 中位 {p50:.2f}ms / p90 {p90:.2f}ms / 均 {avg:.2f}ms ｜ "
            f"ship 合计 {sum(ship_us)/1000.0:.0f}ms，睡眠合计 {sleep_us/1000.0:.0f}ms，"
            f"等TTS {starve_us/1000.0:.0f}ms，让路 {hi_us/1000.0:.0f}ms（{hi_hits} 次），"
            f"墙钟合计 {tot_wall/1000.0:.0f}ms")
        log(f"   队列最低存货 {qmin_ms if qmin_ms < (1<<30) else -1}ms ｜ 缺料 {gaps} 次")
        if avg > chunk_ms * 0.5:
            log("   ⚠️ 单块 ship 就吃掉一半以上预算 —— 瓶颈在编码/发布本身，不是节拍")
        if starve_us / 1000.0 > total_ms * 0.2:
            log("   ⚠️ 大量时间在等 TTS 合成 —— edge-tts 出音比播放慢，"
                "该加大 prebuf_ms 或换本地 TTS")
    return audio_ms, first_ms or 0, total_ms


def main():
    ap = argparse.ArgumentParser(description="用她的声音说话，从板子喇叭放出来")
    ap.add_argument("text", nargs="?", default="", help="要说的话")
    ap.add_argument("--ip", default=DEFAULT_IP, help="板子 IP")
    ap.add_argument("--port", type=int, default=UDP_PORT)
    ap.add_argument("--voice", default=DEFAULT_VOICE)
    ap.add_argument("--no-stream", action="store_true", help="用老路：整段合成完再放")
    ap.add_argument("--keep", action="store_true", help="保留 tts.mp3 / tts.pcm 方便检查")
    args = ap.parse_args()

    if not args.text.strip():
        raise SystemExit("用法：python tools/tts_to_board.py \"要说的话\" [--ip 192.168.1.109]")

    log(f"🗣  「{args.text}」  音色={args.voice}")

    if not args.no_stream:
        speak_streaming(args.text, args.ip, args.port, args.voice)
        return

    # 工作目录放在工作区内，不用 %TEMP%：DSH 沙箱下 ffmpeg 子进程往 AppData\Local\Temp
    # 写文件会直接 Permission denied（父进程能写，子进程被拦），换到工作区就正常。
    tmp = Path(__file__).resolve().parent.parent / ".tmp" / "body-tts"
    tmp.mkdir(parents=True, exist_ok=True)
    mp3 = tmp / "tts.mp3"
    pcm = tmp / "tts.pcm"

    synth(args.text, args.voice, mp3)
    to_pcm(mp3, pcm)
    play_pcm_bytes(pcm.read_bytes(), args.ip, args.port)
    if not args.keep:
        for p in (mp3, pcm):
            try:
                p.unlink()
            except OSError:
                pass


if __name__ == "__main__":
    main()
