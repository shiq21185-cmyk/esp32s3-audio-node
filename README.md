# esp32s3-audio-node

**一块 ESP32-S3，变成一个「网络麦克风 + 网络喇叭」。**

板子只干两件事：把麦克风的声音打成 UDP 包推到网上，把网上来的 UDP 包从喇叭放出来。
没有编解码、没有云端、没有协议栈——**16 kHz / 16 bit / 立体声 裸 PCM，双向**。

智力全在电脑上：语音识别、对话、TTS、音效，随便用什么。板子只管「听见」和「说出」。

```
        ┌──────────────┐   16 kHz PCM over UDP   ┌──────────────────┐
        │  ESP32-S3    │ ──────────────────────► │                  │
        │  INMP441  ───┤        ~512 kbit/s      │   你的程序       │
        │  MAX98357 ◄──┤ ◄────────────────────── │  (Python/Node/…) │
        └──────────────┘                         └──────────────────┘
```

实测：16 kHz 立体声、20 ms 一包、45 秒连续推流 **零丢包**；同一局域网内单向延迟稳定在个位数毫秒级。

---

## 它适合干什么

- 给一个跑在电脑上的 AI 助手配「耳朵和嘴」（本项目就是这么来的）
- 房间拾音 / 远程听音
- 无线音箱、对讲、门铃
- 任何「需要一台放在房间里、能听能说、但算力在别处」的场合

## 它不适合干什么

- 高保真音乐（16 kHz 只够人声）
- 公网直连（UDP 裸流，没有加密、没有重传，请只在可信局域网用）
- 多房间同步播放（没有时钟同步）

---

## 硬件

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3 开发板 / 模块 | 需要 **两个独立 I2S** 控制器（S3 有 I2S0 + I2S1） |
| 麦克风 | INMP441 模块 | 数字 I2S 输出，免模拟前端 |
| 功放 | MAX98357A 模块 | I2S 输入 + 内置 DAC + D 类功放，直推 4Ω/8Ω 喇叭 |
| 喇叭 | 4Ω 3W 小喇叭 | 8Ω 也能用，音量小一点 |
| 电源 | USB-C 5V | 见 [docs/hardware.md](docs/hardware.md) 的电流说明 |

参考实物是一块 61×46 mm 的成品语音板（ESP32-S3-WROOM-1 + INMP441 + MAX98357 + WS2812 指示灯），
**引脚定义和接线表都在 [docs/hardware.md](docs/hardware.md)**。面包板手接也完全一样，只需要改 `firmware/audio-node/main/config.h` 里的六个引脚号。

---

## 快速开始

### 1. 烧固件

需要 ESP-IDF v5.x（本项目在 **v5.3.5** 上验证）。

```bash
cd firmware/audio-node
cp main/secrets.h.example main/secrets.h   # 填你的 Wi-Fi（只支持 2.4 GHz）
idf.py set-target esp32s3
idf.py build flash monitor
```

串口里会打印板子拿到的 IP，记下来：

```
I (1234) audionode: ==================================
I (1234) audionode:   connected, board IP = 192.168.1.109
I (1234) audionode: ==================================
I (1234) audionode: listening on UDP 5005 (0xAA-prefixed = audio, else command)
```

没有 BOOT 键的板子：按住 `IO0` 拉低再点复位，或者让自动下载电路自己干活。

### 2. 电脑端

标准库，零依赖，Python 3.9+：

```bash
cd host/python
python audio_node.py --host 192.168.1.109 ping
python audio_node.py --host 192.168.1.109 record out.wav -t 10
python audio_node.py --host 192.168.1.109 play out.wav
python audio_node.py --host 192.168.1.109 loopback -t 5    # 对着板子说话，0.5 秒后从喇叭听见自己
```

`loopback` 是最快的验收：**一个命令就能同时验证两个方向**。

### 3. 当库用

```python
from audio_node import AudioNode

with AudioNode("192.168.1.109") as node:      # 记得关，不然板子会一直推流
    node.ping()                                # -> True
    node.record_wav("me.wav", seconds=10)      # 麦克风 -> 文件

    for pkt in node.iter_packets(seconds=30):  # 或者自己处理每个 20ms 包
        ...                                    # pkt 是 1280 字节裸 PCM

    node.play_wav("reply.wav", gain=0.8)       # 任意 WAV：8/16/24/32bit、单双声道、任意采样率
```

Node.js 也有一个零依赖客户端（`host/nodejs/audio-node.mjs`）：

```js
import { AudioNode } from "./audio-node.mjs";

const node = new AudioNode("192.168.1.109");
await node.ping();
node.on("audio", (pcm) => { /* 20 ms 的 s16le 立体声 PCM */ });
await node.start();          // 板子开始推流
// ... 30 秒后
node.stop();
await node.recordWav("out.wav", 5);
await node.playPcm(pcm);
```

---

## 协议速查

板子固定监听 **UDP 5005**。往下发的包里，第一个字节是 `0xAA` 就是音频，否则当命令（ASCII）。

| 方向 | 报文 | 说明 |
|---|---|---|
| 电脑 → 板子 | `PING` | 板子回 `PONG` |
| 电脑 → 板子 | `HELLO` | 板子回 `READY`，并开始把麦克风推给你 |
| 电脑 → 板子 | `STOP` | 停止推流 |
| 板子 → 电脑 | 裸 PCM | 1280 字节 = 20 ms，**没有包头** |
| 电脑 → 板子 | `0xAA` + PCM | 640 字节 = 10 ms（多少都行，板子按帧对齐） |

完整细节、时序、丢包处理：[docs/protocol.md](docs/protocol.md)。

---

## 踩过的坑（都已写进代码和文档）

- **I2S 断流会无限重放最后一小段**——听起来就是「喇叭一直在叫同一句」。所以播放任务**永远在写**，没声音就写零。
- **Wi-Fi 省电模式**会让 RTT 从个位数毫秒飘到 70 ms 上下，推流必须关掉。
- **INMP441 的 L/R 脚不接地**，麦克风读出来恒为 0。
- **INMP441 只有左声道有数据**（L/R 接地时），量电平时取左声道。
- 播放到尾音要**补一段静音**，否则最后 20 ms 卡在 DMA 环里。
- Windows 防火墙会拦「接收 UDP」，症状是 `PING` 通但一个音频包都收不到。

更多：[docs/architecture.md](docs/architecture.md) 里的「设计取舍与失败记录」。

---

## 目录结构

```
firmware/audio-node/     固件（ESP-IDF 工程，300 行 C）
  main/config.h          引脚 / 采样率 / 端口，改这里就够了
  main/main.c            Wi-Fi + 两个 I2S + 一个 UDP 循环
host/python/             参考客户端（标准库，零依赖）
  audio_node.py          库 + 命令行
  examples/              录 / 放 / 回环 / 语音触发录音
host/nodejs/             Node.js 客户端（零依赖）
docs/hardware.md         选型、引脚、接线、电源
docs/protocol.md         线协议
docs/architecture.md     架构、线程、延迟、踩坑
```

---

## License

MIT。拿去用，改，卖都行，出问题别找我。

---

## English

`esp32s3-audio-node` turns an ESP32-S3 into a **network microphone and speaker**:
raw 16 kHz / 16-bit / stereo PCM over UDP, both directions, no codec, no cloud.

- Firmware: ESP-IDF 5.x, ~300 lines of C, two I2S peripherals (mic in, amp out).
- Host: zero-dependency Python and Node.js clients with a small, documented API.
- Protocol: one UDP port, four ASCII commands (`PING` / `HELLO` / `STOP`), audio
  packets are `0xAA` + PCM downstream and headerless PCM upstream.

Quick start:

```bash
cd firmware/audio-node && cp main/secrets.h.example main/secrets.h   # edit Wi-Fi
idf.py set-target esp32s3 && idf.py build flash monitor              # note the IP

cd ../../host/python
python audio_node.py --host <board-ip> ping
python audio_node.py --host <board-ip> loopback -t 5                 # hear yourself
```

Docs are in Chinese; the code, comments and serial log are English, and
[docs/protocol.md](docs/protocol.md) is short enough to read even if you do not.
