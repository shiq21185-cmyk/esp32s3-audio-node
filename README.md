# esp32s3-audio-node

**一块 ESP32-S3，变成一个「网络麦克风 + 网络喇叭」。**

板子只干两件事：把麦克风的声音推到网上，把网上来的声音从喇叭放出来。
没有云端、没有协议栈——**16 kHz / 16 bit / 裸 PCM 或 Opus**，双向。

智力全在电脑上：语音识别、对话、TTS、音效，随便用什么。板子只管「听见」和「说出」。

这个仓库里有**两套固件**，按场景挑一套：

| | `firmware/audio-node`（UDP） | `firmware/audio-node-mqtt`（MQTT） |
|---|---|---|
| 网段 | 板子和电脑必须在同一张局域网 | 只要能连上同一个 broker，跨网、4G、别人家的 Wi-Fi 都行 |
| 电脑要入站端口 | 要（Windows 防火墙会拦） | **不要** —— 两边都只做出站连接 |
| 延迟 | 最低（局域网个位数毫秒） | 多一跳，同城 broker 约 +10~30 ms |
| 带宽 | 512 kbit/s 立体声裸 PCM | 上行 Opus 16 kbit/s，下行单声道 PCM 256 kbit/s（因为板子上不做解码，见下） |
| 依赖 | 标准库，零依赖 | 电脑侧 `paho-mqtt`（解码可选 `av`） |
| 适合 | 一台放在房间里的助手 | 要带出门的助手 |

```
        ┌──────────────┐                         ┌──────────────────┐
        │  ESP32-S3    │    UDP:5005             │                  │
        │  INMP441  ───┤ ──────────────────────► │   你的程序       │
        │  MAX98357 ◄──┤ ◄────────────────────── │  (Python/Node/…) │
        └──────────────┘                         └──────────────────┘
                共享同一个 broker 时（MQTT）：
        ┌──────────────┐                         ┌──────────────────┐
        │  ESP32-S3    │ ──publish──► broker ──► │                  │
        │              │ ◄──subscribe─────────── │   你的程序（在家、 │
        └──────────────┘                         │   在公司、在 4G） │
                                                 └──────────────────┘
```

实测：UDP 版 16 kHz 立体声、20 ms 一包、45 秒连续推流 **零丢包**；
MQTT 版跨公网（杭州 Serverless broker）一整轮对话首音 1.2 秒、整轮 6.5 秒。

---

## 它适合干什么

- 给一个跑在电脑上的 AI 助手配「耳朵和嘴」（本项目就是这么来的）
- 房间拾音 / 远程听音
- 无线音箱、对讲、门铃
- 任何「需要一台放在房间里、能听能说、但算力在别处」的场合

## 它不适合干什么

- 高保真音乐（16 kHz 只够人声）
- **UDP 版的**公网直连（裸流，没有加密、没有重传，请只在可信局域网用）
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
**引脚定义和接线表都在 [docs/hardware.md](docs/hardware.md)**。

固件支持两种接法，换板子只改 `main/config.h` 顶部的 `BOARD_PROFILE`，**不用改代码**：

| 板型 | 说明 | 麦克风 SCK/WS/SD | 功放 BCLK/LRC/DIN | 按键 |
|---|---|---|---|---|
| `BOARD_INTEGRATED`（默认） | 一体式语音板（参考板实测引脚） | 4 / 5 / 6 | 16 / 17 / 15 | IO1 |
| `BOARD_BREADBOARD` | GOOUUU 果云版 42 脚开发板 + 面包板 | **5 / 4 / 6** | **15 / 16 / 7** | BOOT (GPIO0) |

⚠️ 两套的 BCLK/WS 是**交叉**的，接反不是声音小，是彻底没声音，而串口日志一切正常。
面包板那种接法的完整说明见 [docs/hardware.md §3.1](docs/hardware.md)。

两套固件（UDP / MQTT）的引脚定义完全一致，所以同一块板子换固件不用改线。

MQTT 版多用一个按键（一头接按键脚、一头接 GND，按下 = 低电平）做「我现在要说」。
不接也能跑，只是 `MODE 1`（随身模式）用不了。

PSRAM：UDP 版不需要；**MQTT 版需要 8 MB 八线（N16R8）** —— 喇叭缓冲 256 KB 和 Opus
编解码任务的栈都放那儿。没有 PSRAM 也能启动（缓冲退回 64 KB），但会看到一条警告。

### 可选：一块屏

MQTT 版还能带一块 **0.96" I2C OLED（SSD1306，128×64）**，走 GPIO41/42：

```
VCC→3V3   GND→GND   SDA→GPIO41   SCL→GPIO42      地址 0x3C
```

接上之后屏上会出现**一张会动嘴的小人 + 底部电平条**（竖装），或者**一张宽脸 +
状态栏（Wi-Fi/MQTT/IP/麦克风）+ 电平条**（横装）。看一眼就知道板子在线没、对端在听没。

**接不接都能跑**：`oled_init()` 先探一次 0x3C，探不到只打一行警告就跳过，
音频和 MQTT 完全不受影响。

配置在 `main/config.h`：`OLED_ENABLE` / `OLED_HEIGHT`（32=0.91"，64=0.96"）/
`OLED_ROT`（0 横装，1 或 3 竖装）。细节和"怎么换成你自己的形象"见
[docs/display.md](docs/display.md)。

---

## 快速开始

### 1. 烧固件

需要 ESP-IDF v5.x（本项目在 **v5.3.5** 上验证）。

**UDP 版**（同一局域网）：

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

**MQTT 版**（跨网）：

```bash
cd firmware/audio-node-mqtt
cp main/secrets.h.example main/secrets.h
# 填 Wi-Fi + MQTT 账号；再改 main/config.h 里的 BROKER_URI 和 TOPIC_BASE
idf.py set-target esp32s3
idf.py build flash monitor
```

```c
#define BROKER_URI  "mqtts://your-broker.example.com:8883"
#define MQTT_USE_TLS 1
#define TOPIC_BASE  "node-1a2b3c4d"    // 随机生成，别用 MAC 派生
```

生成一个随机的 base：

```bash
python -c "import secrets;print('node-'+secrets.token_hex(4))"
```

`TOPIC_BASE` 两端必须完全一致 —— 忘了改是「ping 不通」的头号原因。

⚠️ MQTT 版为了装下 Opus 编码器，app 有 1.1 MB，需要用 `sdkconfig.defaults` 里的
`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`（自带，不用手改）。分区表选错会在链接最后
一步报 `app partition is too small`。

没有 BOOT 键的板子：按住 `IO0` 拉低再点复位，或者让自动下载电路自己干活。

### 2. 电脑端

**UDP 版** —— 标准库，零依赖，Python 3.9+：

```bash
cd host/python
python audio_node.py --host 192.168.1.109 ping
python audio_node.py --host 192.168.1.109 record out.wav -t 10
python audio_node.py --host 192.168.1.109 play out.wav
python audio_node.py --host 192.168.1.109 loopback -t 5    # 对着板子说话，0.5 秒后从喇叭听见自己
```

`loopback` 是最快的验收：**一个命令就能同时验证两个方向**。

**MQTT 版** —— 要 `paho-mqtt`：

```bash
pip install paho-mqtt            # 必需
pip install av                   # 可选：上行是 Opus，解它要用；不装就收不到上行音频

cd host/python
python audio_node_mqtt.py --base node-1a2b3c4d \
    --broker your-broker.example.com:8883 --tls -u <user> -p <pass> ping
python audio_node_mqtt.py ... record out.wav -t 6
python audio_node_mqtt.py ... play out.wav
python audio_node_mqtt.py ... tone -t 1.5      # 先放个测试音，确认喇叭通
```

`ping` 是最快的验收：板子在不在、Wi-Fi 信号、麦克风开没开，一条命令全看到。

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

### UDP 版

板子固定监听 **UDP 5005**。往下发的包里，第一个字节是 `0xAA` 就是音频，否则当命令（ASCII）。

| 方向 | 报文 | 说明 |
|---|---|---|
| 电脑 → 板子 | `PING` | 板子回 `PONG` |
| 电脑 → 板子 | `HELLO` | 板子回 `READY`，并开始把麦克风推给你 |
| 电脑 → 板子 | `STOP` | 停止推流 |
| 板子 → 电脑 | 裸 PCM | 1280 字节 = 20 ms，**没有包头** |
| 电脑 → 板子 | `0xAA` + PCM | 640 字节 = 10 ms（多少都行，板子按帧对齐） |

完整细节、时序、丢包处理：[docs/protocol.md](docs/protocol.md)。

### MQTT 版

四个话题，全 QoS 0，`<base>` 两端必须一致：

| 话题 | 方向 | 内容 |
|---|---|---|
| `<base>/audio/up` | 板子 → 电脑 | `0xAC` 打包的 Opus（3 帧/包），或 `0xAA` 裸 PCM |
| `<base>/audio/down` | 电脑 → 板子 | `0xAA` + 裸 PCM（3840 字节 = 120 ms），也认 `0xAB` / `0xAC` |
| `<base>/cmd` | 电脑 → 板子 | ASCII：`PING` / `MIC 1|0` / `GATE 1|0` / `MODE 0|1` / `ECHO 1|0` |
| `<base>/state` | 板子 → 电脑 | JSON 心跳（30 秒一次，`PING` 会立刻回一条） |

```
0xAA + PCM         裸 PCM
0xAB + frame       单帧 Opus
0xAC + [len][data] 打包的 Opus，可装多帧
```

完整细节（帧格式、state 字段含义、TLS、自建 broker 步骤、实测数据）：
[docs/protocol-mqtt.md](docs/protocol-mqtt.md)。

---

## 踩过的坑（都已写进代码和文档）

- **I2S 断流会无限重放最后一小段**——听起来就是「喇叭一直在叫同一句」。所以播放任务**永远在写**，没声音就写零。
- **Wi-Fi 省电模式**会让 RTT 从个位数毫秒飘到 70 ms 上下，推流必须关掉。
- **INMP441 的 L/R 脚不接地**，麦克风读出来恒为 0。
- **INMP441 只有左声道有数据**（L/R 接地时），量电平时取左声道。
- 播放到尾音要**补一段静音**，否则最后 20 ms 卡在 DMA 环里。
- Windows 防火墙会拦「接收 UDP」，症状是 `PING` 通但一个音频包都收不到。

MQTT 版另有一批（更惨，详见 [docs/architecture-mqtt.md](docs/architecture-mqtt.md)）：

- **`snprintf` 静默截断 JSON** ⇒ 板子在发心跳、电脑侧解析失败被 `except` 吞掉 ⇒ 上层判「板子没开机」，而所有音频指标都是绿的。现在坏包会计数并在 `ping` 里报出来（退出码 3）。
- **`Link` 只订 `state` 会永远录不到声音**：麦克风音频走 `<base>/audio/up`，不订它的话 `record()` 返回空字节，而 `state` 一切正常，看起来像「板子没开麦」。
- **paho 2.x 的 `on_connect` 即使密码错也会进来**，rc 是 `ReasonCode`；不看 rc 就置「已连接」会把认证失败伪装成连上了。
- **托管 broker 按每秒消息条数限流**，不是按带宽。60 ms 一包只能投递 55~60%，120 ms 一包才 100%。
- **`spkb` 统计的字节含静音**，队列空时也在涨，不能当「收到多少音频」用。
- **`sbuf` 单位是字节，÷32 才是毫秒**。
- **client_id 两端必须不同**，同 id 是「后到者踢前者」，表现是「一会儿通一会儿断」。
- **电脑不能一次性把音频灌进 broker**，会瞬间撑满板子缓冲然后整段丢。
- **判掉线不能用「多久没收到音频包」**：室内模式安静时本来就不发包，安静和掉线长得一模一样，只能靠 `PING` 拿到一条**新** state。

---

## 目录结构

```
firmware/audio-node/          UDP 版固件（ESP-IDF 工程，约 300 行 C）
  main/config.h               引脚 / 采样率 / 端口，改这里就够了
  main/main.c                 Wi-Fi + 两个 I2S + 一个 UDP 循环
firmware/audio-node-mqtt/     MQTT 版固件（约 1000 行 C + Opus 编解码）
  main/config.h               broker / topic / 引脚 / 门限 / 缓冲 / 屏幕
  main/main.c                 MQTT 收发 + 门限 + 双模式 + 心跳
  main/codec.c                Opus 编解码封装
  main/oled.c                 0.96" SSD1306 驱动（I2C + 1bpp 帧缓冲 + 画点写字）
  main/ui.c                   10 fps 的绘制任务：状态栏 / 小人 / 电平条
  main/sprite.h               竖屏那张小人的点阵（换自己的形象就改这里）
  main/font5x7.c              5x7 ASCII 字库（横屏状态栏用）
  sdkconfig.defaults          分区表 / PSRAM / Wi-Fi 缓冲，别丢
host/python/
  audio_node.py               UDP 版库 + 命令行（标准库，零依赖）
  audio_node_mqtt.py          MQTT 版库 + 命令行（paho-mqtt）
  examples/                   录 / 放 / 回环 / 语音触发录音
host/nodejs/                  Node.js 客户端（UDP，零依赖）
docs/hardware.md              选型、引脚、接线、电源（两套固件通用）
docs/display.md               屏幕：接线、朝向、界面、换形象、掉线排查
docs/protocol.md              UDP 线协议
docs/protocol-mqtt.md         MQTT 线协议（含自建 mosquitto 步骤）
docs/architecture.md          UDP 版架构、线程、延迟
docs/architecture-mqtt.md     MQTT 版架构、任务划分、限流实测、失败记录
docs/faq.md                   为什么这么设计（UDP 还是 MQTT、各个取舍的来由）
```

---

## License

MIT。拿去用，改，卖都行，出问题别找我。

Opus 编解码用的是乐鑫的 `esp_audio_codec`（Espressif Modified MIT，只能配乐鑫芯片），
组件管理器会在编译时自动下载，不占仓库体积。

---

## English

`esp32s3-audio-node` turns an ESP32-S3 into a **network microphone and speaker**.
16 kHz / 16-bit audio, both directions, no cloud — the board only captures and plays;
all intelligence stays on the host.

Two firmwares, pick by scenario:

- `firmware/audio-node` — **UDP**, raw stereo PCM. Same LAN only, lowest latency,
  zero host dependencies, ~300 lines of C.
- `firmware/audio-node-mqtt` — **MQTT**, works across NAT, 4G, someone else's Wi-Fi.
  Opus upstream (16 kbit/s), raw PCM downstream, ~1000 lines of C.

```
LAN:      board <--UDP:5005--> your program
Any net:  board --publish--> broker --subscribe--> your program
```

Host clients: zero-dependency Python (`host/python/audio_node.py`) and Node.js
(`host/nodejs/audio-node.mjs`) for UDP; `host/python/audio_node_mqtt.py` for MQTT
(needs `paho-mqtt`).

Quick start (UDP):

```bash
cd firmware/audio-node && cp main/secrets.h.example main/secrets.h   # edit Wi-Fi
idf.py set-target esp32s3 && idf.py build flash monitor              # note the IP

cd ../../host/python
python audio_node.py --host <board-ip> ping
python audio_node.py --host <board-ip> loopback -t 5                 # hear yourself
```

Quick start (MQTT):

```bash
cd firmware/audio-node-mqtt && cp main/secrets.h.example main/secrets.h
# fill in Wi-Fi + broker credentials, then set BROKER_URI and TOPIC_BASE in main/config.h
idf.py set-target esp32s3 && idf.py build flash monitor

pip install paho-mqtt            # 'av' too if you want to decode upstream Opus
cd host/python
python audio_node_mqtt.py --base <base> --broker <host>:8883 --tls -u <user> -p <pass> ping
```

Docs are in Chinese; the code, comments and serial log are English, and
[docs/protocol.md](docs/protocol.md) / [docs/protocol-mqtt.md](docs/protocol-mqtt.md)
are short enough to read even if you do not.
