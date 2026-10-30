#!/usr/bin/env python
"""跟小鲸鱼说话（用身体，不是 QQ）—— 走公网 MQTT，不在同一个网也能用。

一条完整的回路：
    你说话 ──板子麦克风──> MQTT(broker) ──> 本脚本 ──> ASR(她的 faster-whisper) ──> 文字
                                                          │
                                                          ▼
                                                   她的脑子（DSH agent）
                                                          │
                       板子喇叭 <── MQTT(broker) <── PCM <── TTS(她的 edge-tts 音色)
                          ▲
                          └── 她开口

⚠️ 前提：板子在线（d8-mqtt-audio 固件）、双方都能上公网；
   DSH 在 127.0.0.1:19387 跑着；她的语音工具在 qq-bridge-deploy/tools/asr。

用法（用她那个 venv 的 python 跑：edge_tts / faster-whisper 都在那）：
    ...\\tools\\asr\\.venv\\Scripts\\python.exe tools/body_talk.py
    ... tools/body_talk.py --max 20 --silence 1.5
    ... tools/body_talk.py --text "不用说话，直接让她开口"
    ... tools/body_talk.py --base whale-a90c6c --broker broker.hivemq.com
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import body_config   # noqa: E402  （控制台那份可调参数，见 tools/body_config.py）
import body_mqtt     # noqa: E402  （公网中转那条链路，见 tools/body_mqtt.py）
import body_opus     # noqa: E402  （上行一帧多长，见 tools/body_opus.py）

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
_T2S = None   # None=还没初始化 / False=没装 opencc / 实例=可用
TMP = ROOT / ".tmp"
BRIDGE = Path(r"C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy")
BRIDGE_PORT = 3100
ASR_DIR = BRIDGE / "tools" / "asr"
VENV_PY = ASR_DIR / ".venv" / "Scripts" / "python.exe"
WORKER = ASR_DIR / "worker.py"

SAMPLE_RATE = body_mqtt.SAMPLE_RATE
# ⚠️ 这里量的是**上行**：link.recv() 交出来的是「解码后的一帧」，一帧
# FRAME_MS=60ms（见 body_opus.py）。别拿 body_mqtt.CHUNK_MS —— 那是
# **下行**发一包的长度，10-28 改成 120ms 之后两者就不再相等了，
# 用它会把所有"多少包算 0.5 秒有声"的阈值算小一半。
PKT_S = body_opus.FRAME_MS / 1000.0   # 一包音频有多长（0.06 秒）


def log(msg):
    print(msg, flush=True)


def ms(t0):
    return int((time.time() - t0) * 1000)


# ---------------------------------------------------------------- 录音
def record(link, *, max_s=25.0, silence_s=2.0,
           wav_out=None, wait_s=4.0, cue=False):
    """从板子录一段话，再**离线**判断这段里到底有没有人说话。

    为什么判据放在录完之后而不是录的过程里：
      在线判断必须在几百毫秒内做决定，阈值一定粗糙 ——
      快了会把噪声当人声，慢了会把人声漏掉（之前就是这么丢的）。
      录完再判，可以拿整段的分布慢慢算，几乎不会错。
    在线只做一件很宽松的事：判断"好像有人在说"，用来决定什么时候收尾。

    链路规矩（d8-mqtt-audio）：发 "MIC 1" 板子才开始往 <base>/audio/up 推包，
      发 "MIC 0" 停。PCM = 16000Hz / 16bit / **单声道**。
    麦克风默认关着（公共 broker 上不该一直传屋里声音），所以这里先开、录完关。
    """
    if cue:
        try:
            link.beep()
            time.sleep(0.25)           # 等这声"叮"放完，别被麦克风录进去
        except Exception:
            pass

    link.mic(True)
    time.sleep(0.15)
    link.drain()                       # 上一轮她说话时可能还积着包，扔掉
    log("🎧 在听了 —— 你说话吧" + ("（听到\"叮\"就可以说了）" if cue else ""))

    # 刚开麦时 I2S 有时会吐一整片 ±32768（DMA 还没填上）。
    # 早先的写法是「开麦后 0.3 秒整段不收」—— 问题是真人听到"叮"马上开口，
    # 头几个字就正好落在这 0.3 秒里被吞掉（2026-10-28 巡检发现）。
    # 现在改成只看包本身：`extreme > 0.95`（几乎每颗样本都贴着满量程）才丢，
    # 那是 DMA 垃圾的独有特征，真人声再响也是参差的。
    # 实测（GATE 0 关掉人声门限、屋里安静）28 包全是 peak 12~40、extreme 0.000，
    # 没有一片垃圾 —— 说明按包判就够了，不需要按时间一刀切。
    frames, peaks = [], []
    t0 = time.time()
    last_pkt = t0          # 上一个包到的时刻：板子掉线/卡顿时靠它止损
    end_why = ""
    t_voice = None
    last_voice = None
    live_run = 0
    voiced_n = 0

    # 上游一包 60ms（body_opus.FRAME_MS），"攒够 0.5 秒有声"= 8 包。
    # OPEN_RUN 曾经是 2（要连续两包才算开口），但配合过高的门槛会变成：
    # 一句话里几乎没有"连续两包都过门槛"，于是 t_voice 一直不开 → 永远在等。
    # 门限已经在固件里做过一次，这里一包就算开口。
    OPEN_RUN = 1
    MIN_VOICED = max(4, int(round(0.5 / PKT_S)))

    # 板子空闲时 30 秒才发一条 state（省流量），所以"安静"和"掉线"在数据流上
    # 长得一模一样。每隔 15 秒主动敲一次 PING，两次不回才算掉线 ——
    # 这样发现掉线只要 20 秒左右，而心跳可以放心停在 30 秒。
    PROBE_EVERY = 15.0
    last_probe = t0
    offline_hits = 0

    # 断流阈值：必须**远大于**板子的人声门限挂留（GATE_HANG_MS = 1.2 秒）。
    # 门限一关板子就停止发包，"安静"和"掉线"在数据流上长得一样 ——
    # 所以只有真的一句完整的话（voiced_n >= MIN_VOICED）之后还断流这么久，
    # 才算链路出问题。
    STALL_S = 6.0
    # 假开头的清理线：桌面响一下、椅子嘎一声也能顶起 t_voice，
    # 有声量不够就当作没开口，清空重等。
    RESET_QUIET = max(silence_s + 1.0, 3.0)

    def judge(now):
        """返回 (原因, 动作)；动作 in ('stop', 'reset', None)。

        为什么抽成函数：**收工判断必须在"没收到包"的分支里也跑**。
        旧代码把静音判断写在收到包之后，可你说完停嘴时板子正好不发包，
        于是那条路永远走不到，只剩一条写死的 1.5 秒"掉线"判断在生效 ——
        症状就是话被从中间截断，或者明明没人说话却报掉线。
        """
        nonlocal t_voice, last_voice, voiced_n, live_run
        if t_voice is not None and (now - t_voice) > max_s:
            return f"到 {max_s:.0f} 秒上限（可能你还没说完）", "stop"
        if last_voice:
            quiet = now - last_voice
            if quiet > silence_s and voiced_n >= MIN_VOICED:
                return f"静音 {quiet:.1f} 秒", "stop"
            if quiet > RESET_QUIET and voiced_n < MIN_VOICED:
                return None, "reset"
        if (t_voice is not None and voiced_n >= MIN_VOICED
                and (now - last_pkt) > STALL_S):
            return f"断流 {now - last_pkt:.1f} 秒（板子没再发包，可能掉线）", "stop"
        return None, None

    try:
        while True:
            pkt = link.recv(0.3)
            now = time.time()

            if pkt is None:
                reason, action = judge(now)
                if action == "stop":
                    end_why = reason
                    break
                if action == "reset":
                    # ⚠️ 绝不能 frames.clear() —— 那是把已经录到的人声扔掉。
                    # 2026-10-28 就栽在这：门槛一旦偏高，每 3 秒 reset 一次，
                    # 每次把前面积攒的音频清空，最后只剩最后 1 秒多交给 ASR，
                    # 用户听起来就是"话没说完就被截断了"。
                    # reset 只该重置"判断状态"，音频一律留着。
                    t_voice = None
                    last_voice = None
                    voiced_n = 0
                    live_run = 0
                    last_pkt = now
                    continue
                if t_voice is None and (now - t0) > wait_s:
                    log(f"   ……{wait_s:.0f} 秒没听到动静，先停")
                    break
                # 只是没包 —— 可能屋里本来就安静，也可能板子没了。
                # 敲一下 PING：能立刻回一条 state 就是活着。
                if (now - last_probe) > PROBE_EVERY:
                    last_probe = now
                    if link.alive(3.0):
                        offline_hits = 0
                    else:
                        offline_hits += 1
                        log(f"   ……PING 没回应（第 {offline_hits} 次）")
                        if offline_hits >= 2:
                            end_why = "板子不在线（PING 两次没回应）"
                            break
                continue

            last_pkt = now
            if not pkt:
                continue

            peak, extreme = body_mqtt.Link.peak_of(pkt)
            # 全是 ±32768 ⇒ DMA 没填上的垃圾，不是声音。
            # 真人声就算很响也是参差的（有从 300 到 30000 的各种值），
            # 只有硬件毛刺才是"每一颗都贴着满量程"。
            if extreme > 0.95:
                continue
            frames.append(pkt)
            peaks.append(peak)

            # ⚠️ 在线门槛的坑（2026-10-28 血泪）：
            # 固件的人声门限已经把静音挡在外面了 —— 到这里的 peaks 里**全是说话**，
            # 一点底噪都没有。拿这堆数字算"第 20 百分位当底噪、再乘 4"，
            # 等于拿"人声自己"当底噪、再要求比它响 4 倍：
            #   实测一段 21 包的真话，20 百分位 = 147 → 门槛被顶到 588，
            #   而这段话的峰值只有 80~623，结果**只有 2 包能过**。
            # 于是 voiced_n 永远凑不够、last_voice 永远不更新（静音判断失效），
            # RESET_QUIET 一到就 reset 把已经录到的 frames 全清掉 ——
            # 用户听到的就是"我还没说完，录音就结束了"，而且结尾总是只剩一小段。
            # 正确做法：门槛只用来挡"真的什么都没有"，绝对压低。
            if len(peaks) >= 12:
                ordered = sorted(peaks)
                base = max(ordered[int(len(ordered) * 0.20)], 20)
            else:
                base = 40
            thr_live = max(120.0, base * 1.5)
            if peak > thr_live:
                live_run += 1
                voiced_n += 1
                if live_run >= OPEN_RUN:
                    if t_voice is None:
                        t_voice = now
                    # last_voice 只认"连续两包有声"，零星的几包噪声不让它续命 ——
                    # 否则你说完一句话，桌面轻响一下又把收尾计时器刷新一次。
                    last_voice = now
            else:
                live_run = 0

            reason, action = judge(time.time())
            if action == "stop":
                end_why = reason
                break
            if action == "reset":
                # 同上：只重置判断状态，已录到的音频一个字都不许丢。
                t_voice = None
                last_voice = None
                voiced_n = 0
                live_run = 0
                continue
            if t_voice is None and (time.time() - t0) > wait_s:
                log(f"   ……{wait_s:.0f} 秒没听到动静，先停")
                break
    finally:
        link.mic(False)     # 一定要关：不关就一直往公网推屋里声音

    if end_why:
        log(f"   （收工原因：{end_why}）")

    if not peaks:
        log("🔇 一个包都没收到")
        return None, 0.0

    # ---------------- 离线判定 ----------------
    # 同样理由（见上面在线门槛那段注释）：peaks 里全是说话、没有静音，
    # 用它算"底噪"再乘 4，就是把真话判成"没人说话"。
    # 这里只需要回答"这段里到底有没有信号"，门槛压到最低。
    ordered = sorted(peaks)
    floor = max(ordered[int(len(ordered) * 0.20)], 40)
    thr = max(120.0, floor * 1.5)
    hot = [k for k, p in enumerate(peaks) if p > thr]
    clip_max = max(peaks)

    if not hot:
        log(f"🔇 没人说话（最大 {clip_max}，底噪 {floor}，门槛 {int(thr)}，{len(peaks)} 包）")
        return None, 0.0

    lo = max(0, hot[0] - 2)
    hi = min(len(frames), hot[-1] + 3)
    span = hi - lo
    voiced_sec = len(hot) * PKT_S
    if voiced_sec < 0.15 or (span < 4 and clip_max < 1500):
        log(f"🔇 太短，不算说话（{span} 包，最大 {clip_max}）")
        return None, 0.0

    audio = b"".join(frames[lo:hi])
    audio = audio[: len(audio) - (len(audio) % body_mqtt.FRAME_BYTES)]

    dur = len(audio) / body_mqtt.FRAME_BYTES / SAMPLE_RATE
    log(f"⏹  录到 {dur:.2f}s（最大 {clip_max}，有声 {voiced_sec:.2f}s，底噪 {floor}）")
    if wav_out is None:
        wav_out = TMP / "hear.wav"
    wav_out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(wav_out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(audio)
    return wav_out, dur


# ---------------------------------------------------------------- 听写
class Asr:
    """常驻 worker：模型只加载一次（约 3.5 秒），之后每段话零点几秒。"""

    def __init__(self):
        self.proc = None
        self.seq = 0

    def start(self):
        if self.proc:
            return
        t = time.time()
        self.proc = subprocess.Popen(
            [str(VENV_PY), str(WORKER)],
            cwd=str(ASR_DIR),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
            env={**os.environ, "PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1",
                 # beam=1：不找最优解只走贪心。中文短句里 beam 5 换来的一点点准确率
                 # 不值那一秒多的延迟 —— 对话里晚一秒比听错一个字难受得多。
                 "ASR_BEAM": os.environ.get("ASR_BEAM", "1")},
        )
        line = self.proc.stdout.readline()
        try:
            ready = json.loads(line)
        except Exception:
            raise SystemExit(f"❌ ASR worker 起不来：{line[:300]}")
        if not ready.get("ready"):
            raise SystemExit(f"❌ ASR 模型加载失败：{ready.get('error')}")
        log(f"   ASR 就绪（model={ready.get('model')}, {ready.get('device')}，{ms(t)}ms）")

    def transcribe(self, wav):
        self.start()
        self.seq += 1
        req = {"id": self.seq, "wav": str(wav), "language": "zh"}
        self.proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise SystemExit("❌ ASR worker 断了")
            try:
                rep = json.loads(line)
            except Exception:
                continue
            if rep.get("id") != self.seq:
                continue
            if not rep.get("ok"):
                raise SystemExit(f"❌ 转写失败：{rep.get('error')}")
            return rep.get("text", ""), rep.get("elapsed_ms", 0)

    def close(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.write('{"id":999,"shutdown":true}\n')
            self.proc.stdin.flush()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
        self.proc = None


# ---------------------------------------------------------------- 她的脑子
def reset_her_session():
    """把「她跟主人那条会话」的上下文清掉（她会失忆，但记忆条目留着）。

    走桥接的 POST /api/session/reset —— 这是控制台上「清上下文」按钮用的同一个
    端点：删掉 key→sessionId 映射、归档旧会话，重新开口时 ensureSession()
    会新建一个。话题/想法/印象/约定这些 socialV2 记忆是**保留**的
    （2026-10-23 主人定的口径：清上下文不删记忆）。

    ⚠️ 这条以前是 `--new` 参数，但根本没接上任何东西 —— 传了也照样是原来那段
    会话，帮助文本却写着「她会失忆」。2026-10-28 巡检发现，接上了。
    """
    import urllib.request
    import urllib.error

    tok = BRIDGE / "state" / "console-token"
    token = tok.read_text(encoding="utf-8").strip() if tok.exists() else ""
    cfgf = BRIDGE / "config.json"
    key = ""
    try:
        key = str((json.loads(cfgf.read_text(encoding="utf-8")).get("call") or {})
                  .get("answerKey") or "")
    except Exception:
        key = ""
    if not key:
        raise RuntimeError("config.json 里没有 call.answerKey，不知道该清哪条会话")
    req = urllib.request.Request(
        f"http://127.0.0.1:{BRIDGE_PORT}/api/session/reset",
        data=json.dumps({"key": key}).encode("utf-8"),
        headers={"content-type": "application/json", "x-console-token": token},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")[:200]
        raise RuntimeError(f"清会话失败 HTTP {exc.code}：{body}") from exc
    except Exception as exc:
        raise RuntimeError(f"连不上桥接（127.0.0.1:{BRIDGE_PORT}）：{exc}") from exc
    if not data.get("ok"):
        raise RuntimeError(data.get("error") or "桥接没清成功")
    return key, data.get("archived") or ""


def ask_her(text, new=False):
    """把她的话送进**她跟主人 QQ 私聊那一条会话**，只取这一轮的回复文本。

    走桥接的 /api/body/turn：
      · 同一个 session —— 板子上说话的她和 QQ 里打字的是同一个人、同一段记忆；
      · 桥接会给这一轮挂"静默名额"，所以回复不会跑进 QQ 聊天窗；
      · 上下文由桥接拼（屋里模式 + 当前时间），这里不要再自己加时间前缀。
    new=True 时先清掉那条会话的上下文（= 她会失忆，记忆条目保留）。
    返回她的回复；失败抛 RuntimeError。
    """
    import urllib.request

    if new:
        key, archived = reset_her_session()
        log(f"🧹 已清空会话上下文（{key}，旧会话 {archived or '无'} 已归档）—— 她这轮会失忆")
        time.sleep(0.3)      # 让桥接把状态落盘，紧接着开口才不会撞上旧会话

    tok = BRIDGE / "state" / "console-token"
    token = tok.read_text(encoding="utf-8").strip() if tok.exists() else ""
    payload = json.dumps({"text": text, "timeoutMs": 90000}).encode("utf-8")
    req = urllib.request.Request(
        f"http://127.0.0.1:{BRIDGE_PORT}/api/body/turn",
        data=payload,
        headers={"content-type": "application/json", "x-console-token": token},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        raise RuntimeError(f"连不上她的桥接（127.0.0.1:{BRIDGE_PORT}）：{exc}") from exc
    if not data.get("ok"):
        raise RuntimeError(data.get("error") or "她这一轮没回话")
    return (data.get("reply") or "").strip()


# ---------------------------------------------------------------- 开口
def to_simplified(text):
    """whisper 的中文会时好时坏地吐繁体（实测「你好你是誰」）。这里过一遍 t2s。

    opencc 没装就原样返回 —— 这是锦上添花，不该让它把整条链路搞挂。
    简体化只动字形、不动意思，所以先转再给她看，不会改变她想表达的内容。
    """
    global _T2S
    if not text:
        return text
    if _T2S is None:
        try:
            from opencc import OpenCC
            _T2S = OpenCC("t2s")
        except Exception:
            _T2S = False
    if _T2S is False:
        return text
    try:
        return _T2S.convert(text)
    except Exception:
        return text


def now_prefix():
    """她读不到系统时钟（实测问她几点，她答"我这儿没表"），所以每句话前面挂一个。

    括号里是背景信息，不是你说的话 —— 提示词里已经交代过，
    她照着回答就好，不会把它念出来。
    """
    t = datetime.datetime.now()
    week = "一二三四五六日"[t.weekday()]
    return f"〔现在是 {t:%Y-%m-%d %H:%M} 周{week}〕"


def shorten(text, cap=110):
    """兜底限长：她要是又写一大段，就在句号处截断。

    中文正常语速一秒四五个字，110 字差不多 20 秒——再长就不像"回话"了。
    优先切在句号/问号/叹号后面，切不到再切逗号，实在没有标点才硬截。
    """
    text = text.strip()
    if len(text) <= cap:
        return text
    head = text[:cap]
    for mark in ("。", "！", "？", ".", "!", "?"):
        i = head.rfind(mark)
        if i >= cap * 0.5:
            return head[: i + 1]
    for mark in ("，", "、", ",", "；", ";"):
        i = head.rfind(mark)
        if i >= cap * 0.5:
            return head[:i] + "。"
    return head + "…"


def speak(text, link, volume=1.0):
    """边合成边播：第一块音频到位就开始出声，不等整段合成完。

    volume 是**播放增益**（0~1.5，1.0=原始）：板子上没有硬件音量（MAX98357 的
    GAIN/SD 都悬空），所以音量只能在 PCM 上乘一下。控制台里那个滑杆改的就是它。
    """
    # 2026-10-27 血泪（第 N 轮，终于有硬证据）：
    #   用固件自报的"写进 I2S 的字节数"量出来，长句播放期间板子确实 100% 实时，
    #   但**到了句尾会连续写好几段静音**（实测最后一秒 32%~50% 是静音）。
    #   原因不是链路、不是编码、不是 I2S 采样率（实测 16k 整点），而是：
    #   TTS 合成到头了 → 队列空 → 段间垫静音被真的读走了。
    #   也即：板子"前面播得满、后面没料"，正是用户说的"前几个字清楚，后面一卡一卡"。
    #
    #   之前几轮的"按水位自适应/94% 节拍"都在治标：填得太满会把缓冲顶到 2048 上限，
    #   一旦溢出就丢帧（杂音）；填得太少就饿死。
    #
    # 这一版换成"段缓冲"（jitter buffer）：
    #   先把整段音频在 PC 上合成完、编码到 Opus 包，再一次性灌给板子。
    #   代价是开口前多等 1~2 秒（一段话的合成时间），
    #   换来全程不受网络抖动与合成速度影响 —— 这是"带出门用"必须付出的。
    from tts_to_board import speak_streaming, DEFAULT_VOICE
    text = shorten(text)
    # 下行编码每句话重新读一次控制台配置 —— 这样在网页上切 PCM/Opus
    # **不用重启这个窗口**，下一句就生效（出问题也能立刻切回来）。
    try:
        want = str(body_config.load(fresh=True).get("downCodec", "pcm")).lower()
        if want != getattr(link, "down_codec", "pcm"):
            link.down_codec = "opus" if want == "opus" else "pcm"
            log(f"   🔉 下行编码已切成 {link.down_codec.upper()}")
    except Exception:
        pass
    # 句首敲一次 PING 留基线，句尾再敲一次，用板子自报的计数器做体检
    s0 = link.snapshot() if hasattr(link, "snapshot") else {}
    try:
        return speak_streaming(text, voice=DEFAULT_VOICE, gain=volume,
                               sink=link.send_audio, channels=1,
                               chunk_bytes=body_mqtt.CHUNK_BYTES,
                               chunk_ms=body_mqtt.CHUNK_MS,
                               # 先垫一段给板子打底（2026-10-27 第九轮重定）：
                               # · 板子缓冲已从 64KB 加到 256KB = 8192ms（第九修）
                               # · 所以垫底可以大胆一点：2500ms，让板子开口前就有
                               #   2.5 秒存货，之后 PC 只要平均追平就永远不见底
                               # · 不能再有"paho 一次性冲出顶满"的问题：8 秒缓冲
                               #   一次冲 5 包（1200ms）只占 15%
                               lead_ms=2500,
                               # 按板子缓冲水位发：固定 60ms 节拍实测慢 13ms/块，
                               # 87 块累计慢 1.1 秒，把垫底吃到只剩 0.2 秒，
                               # 听感就是"前几个字清楚、后面一卡一卡"。
                               level=link.sbuf_ms,
                               # 2026-10-27 第四轮：用户实机"后面一卡一卡"，我本地
                               # 怎么测都顺 —— 因为堵的在他那儿（VPN/TLS 抖动）。
                               # 交给它**本地链路计数器**，堵的时候自动放慢节拍。
                               health=link.health,
                               # 2026-10-27 第六轮（终于有硬证据）：
                               #   用固件自报的"I2S 实际写入字节数"量出来 ——
                               #   板子播放速率 16000Hz 整点（100.0% / 99.9%）；
                               #   播放期间逐秒静音占比 0%，缓冲 1900~2048ms 始终满；
                               #   **只有句尾一到两秒是纯静音**：队列空了，没料了。
                               #   ⇒ 根因是"送料 vs 播放"对不齐，句尾必然见底。
                               # 所以放弃边合成边送，改成整句做齐再匀速推。
                               # 代价：开口前多等 1~2 秒；换来一句到底不断。
                               full_prebuf=True)
    finally:
        # 最后不足一帧的零头必须挤出来，否则每句话结尾会吞掉几十毫秒
        # （听感上是句尾"咔"一下截断）。只在 Opus 通路上有意义。
        try:
            link.send_audio_flush()
        except Exception:
            pass
        # ---- 一句说完了，让板子自己汇报：到底播了多久、其中多少是静音 ----
        # 注意：这个窗口包含了"开口前那 1~2 秒的等待"，板子闲着也在写静音，
        # 所以静音占比天然会被抬高。真正该看的是 ddrop（溢出丢帧）。
        try:
            s1 = link.snapshot() if hasattr(link, "snapshot") else {}
            if s0 and s1 and "spkb" in s1:
                # ⚠️ 两个量纲别混：
                #   spkb/spks —— 固件写 I2S 的字节数，I2S 是**立体声** 16bit
                #                ⇒ 64 字节 = 1ms；
                #   sbuf       —— 喇叭 StreamBuffer 里的字节数，那是**单声道**
                #                ⇒ 32 字节 = 1ms。
                # 2026-10-28 之前这里还留着一个算完没人用的 idle，已删。
                played = (s1["spkb"] - s0.get("spkb", 0)) / 64.0
                sil = (s1.get("spks", 0) - s0.get("spks", 0)) / 64.0
                drop = s1.get("ddrop", 0) - s0.get("ddrop", 0)
                if played <= 0 and sil <= 0:
                    # snapshot() 超时会拿"上一条 state"兜底，两个差值都是 0。
                    # 这时候把 0ms 当"板子没出声"读就完全错了 —— 是没拿到新数据。
                    log("   🩺 板子侧：这轮没拿到新心跳（PING 没回），"
                        "体检数字不可信 —— 不影响这一句的播放")
                else:
                    log(f"   🩺 板子侧：窗口内写 I2S {played:.0f}ms，静音 {sil:.0f}ms"
                        f" ｜ 溢出丢帧 {drop} ｜ 残留 buff {s1.get('sbuf', 0)/32:.0f}ms")
                    if drop > 0:
                        log("   ⚠️ 有溢出丢帧 —— 送太快或垫底太多，会断字")
        except Exception:
            pass


# ---------------------------------------------------------------- 主流程
def main():
    import body_config as bcfg
    cfg = bcfg.load()
    ap = argparse.ArgumentParser(description="用身体跟小鲸鱼说话（走公网 MQTT）")
    ap.add_argument("--base", default=None,
                    help="主题前缀 whale-<MAC 后三字节>（缺省读控制台那份）")
    ap.add_argument("--broker", default=None, help="MQTT broker 地址（缺省读控制台那份）")
    # 这四个默认值来自控制台那份 body-config.json；命令行显式给了就以命令行为准。
    ap.add_argument("--max", type=float, default=None,
                    help=f"最多录几秒（默认取控制台设置 {cfg['maxSec']:g}）")
    ap.add_argument("--silence", type=float, default=None,
                    help=f"停顿多久算说完（默认取控制台设置 {cfg['silenceSec']:g}）")
    ap.add_argument("--wait", type=float, default=None,
                    help=f"一直没人说话就放弃（默认取控制台设置 {cfg['waitSec']:g}）")
    ap.add_argument("--no-cue", action="store_true", help="不放提示音（压过控制台设置）")
    ap.add_argument("--text", default="", help="跳过录音，直接拿这段文字去问她")
    ap.add_argument("--new", action="store_true", help="重开一段会话（她会失忆）")
    ap.add_argument("--loop", action="store_true", help="连续对话，说完一句接下一句")
    ap.add_argument("--rounds", type=int, default=3, help="--loop 时最多几轮（0 = 一直听）")
    args = ap.parse_args()
    log(f"⚙️  控制台设置：音量 {cfg['volume']:g} ｜ 停顿 {cfg['silenceSec']:g}s ｜ "
        f"单句上限 {cfg['maxSec']:g}s ｜ 提示音 {'开' if cfg['cue'] else '关'} ｜ {bcfg.path()}")

    def setting(name, key):
        """命令行给了就用命令行，否则每一轮都重新读控制台那份。"""
        explicit = getattr(args, name)
        if explicit is not None:
            return explicit
        return bcfg.load(fresh=True)[key]

    TMP.mkdir(parents=True, exist_ok=True)

    # 主题前缀和 broker 也允许控制台改：命令行没给就跟着控制台走。
    args.base = setting("base", "base")
    args.broker = setting("broker", "broker")
    # 端口 / TLS / 账号也从同一份配置取 —— 否则切 EMQX 时这里会拿着默认端口。
    kw = bcfg.link_kwargs(fresh=True)
    kw["base"] = args.base
    kw["broker"] = args.broker
    log(f"📡 连 broker {args.broker}:{kw['port']}"
        f"{' (TLS)' if kw['use_tls'] else ''}，前缀 {args.base} …")
    log(f"   🔉 下行编码 {kw['down_codec'].upper()}"
        f"（{'约 2KB/s，板子现场解码' if kw['down_codec'] == 'opus' else '32KB/s，板子只搬运'}）")
    try:
        link = body_mqtt.Link(**kw).connect()
    except RuntimeError as exc:
        raise SystemExit(f"❌ {exc}")
    st = link.wait_state(6.0)
    if st:
        log(f"   ✅ 板子在线（rssi {st.get('rssi')}，堆 {st.get('heap')}，IP {st.get('ip')}）")
    else:
        log("   ⚠️ 没看到板子的心跳 —— 先照样跑，板子一上线就自动接着听")

    # 模式自检：控制台记的意图 vs 板子实际存的模式。不一致就推一次（板子存 NVS）。
    want_mode = bcfg.load(fresh=True).get("boardMode", "gate")
    if st and st.get("mode") is not None:
        have = "btn" if st.get("mode") == 1 else "gate"
        if have != want_mode:
            log(f"   🔧 板子当前是{'随身' if have == 'btn' else '室内'}，控制台要的是"
                f"{'随身' if want_mode == 'btn' else '室内'}，推一次模式…")
            n0 = link._state_n
            link.client.publish(link.t_cmd, f"MODE {1 if want_mode == 'btn' else 0}", qos=0)
            t0 = time.time()
            target = 1 if want_mode == "btn" else 0
            while time.time() - t0 < 6 and link.state.get("mode") != target:
                time.sleep(0.05)
            log("   ✅ 已切过去" if link.state.get("mode") == target
                else "   ⚠️ 板子没确认，先照样跑")
        else:
            log(f"   模式：{'随身（按键说话）' if want_mode == 'btn' else '室内（人声门限）'}")

    asr = Asr()
    # 提前把模型拉起来：加载要 3~20 秒，正好趁它加载的时候开始听你说。
    try:
        asr.start()
    except SystemExit as e:
        link.close()
        log(str(e))
        raise
    first = True
    try:
        if args.text:
            one_round(asr, args, link, args.text, new_flag=args.new)
            return
        # --rounds 0 当无限用：身体就该一直在那儿听。
        total_rounds = (args.rounds if args.rounds > 0 else 10 ** 9) if args.loop else 1
        silent_streak = 0   # 连续几轮一个包都没收到（板子掉线的判断依据）
        for r in range(total_rounds):
            if args.loop:
                log(f"── 第 {r + 1} 轮 ──")
            try:
                wav, dur = record(link, max_s=setting("max", "maxSec"),
                                  silence_s=setting("silence", "silenceSec"),
                                  wait_s=setting("wait", "waitSec"),
                                  cue=(not args.no_cue) and bcfg.load(fresh=True)["cue"])
            except KeyboardInterrupt:
                break
            if wav is None:
                # 板子掉线时不要疯狂刷屏：提示一次，之后每轮退避 5 秒。
                silent_streak += 1
                if silent_streak == 3:
                    log(f"⚠️  连续 {silent_streak} 轮一个包都没收到 —— 板子可能掉线了"
                        f"（{args.base}）")
                    log("    还在试，板子重新上线就自动接着听。")
                if silent_streak >= 3:
                    time.sleep(5)
                continue
            silent_streak = 0
            t0 = time.time()
            user_text, asr_ms = asr.transcribe(wav)
            user_text = to_simplified(user_text)
            log(f"📝 听到（{asr_ms}ms）：「{user_text}」")
            if not user_text.strip():
                log("（没听清，这次算了）")
                continue
            one_round(asr, args, link, user_text, already_text=True,
                      new_flag=args.new and first)
            first = False
    except KeyboardInterrupt:
        log("\n（结束）")
    finally:
        try:
            link.mic(False)
        except Exception:
            pass
        link.close()
        asr.close()


def one_round(asr, args, link, user_text, *, already_text=False, new_flag=False):
    t_all = time.time()
    if not already_text:
        log(f"⌨️  直接问：「{user_text}」")
    t0 = time.time()
    reply = ask_her(user_text, new=new_flag)
    log(f"💭 她想了 {ms(t0)}ms：「{reply}」")
    if not reply.strip():
        log("（她没说话）")
        return
    t0 = time.time()
    _audio_ms, first_ms, _total_ms = speak(reply, link, volume=body_config.load()["volume"])
    log(f"🔊 首音 {first_ms}ms ｜ 念完 {ms(t0)}ms ｜ 本轮 {ms(t_all)}ms")


if __name__ == "__main__":
    main()
