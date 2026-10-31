# 小鲸鱼的身体

给一个跑在 PC 上的 AI 人格（她叫**小鲸鱼**，通过 QQ 说话）做一副**物理身体**：
ESP32-S3 + FreeRTOS + TTL 总线舵机 + 麦克风/喇叭 + 小屏 + 摄像头。

**智力留在 PC，板子只做两件事**：本地反射（<50ms，不走网络）+ 表现层（把"开心"演成动作）。

> 这是一个个人项目，硬件是面包板 + 现成模块堆出来的，不是产品。
> 代码里的注释比文档长，因为每次踩坑都写回去了——那些坑比结论值钱。

---

## 现在能做什么

**`firmware/e5-body-cam`（⭐ 主力固件）** —— 一块板子上有耳朵、嘴、屏幕和眼睛：

| 能力 | 实现 |
|---|---|
| 耳朵 | INMP441 → I2S 采集 → Opus 编码 → MQTT 上行 `<base>/audio/up` |
| 嘴 | MQTT 下行 `<base>/audio/down` → 8 秒喇叭缓冲 → MAX98357 |
| 表情 | 0.96" I2C OLED（64×128 竖装），自己画的点阵小人，状态栏显示网络/音量 |
| 眼睛 | OV2640（JPEG/VGA）→ 拍照 / 推流 → `<base>/cam/jpeg` |
| 出门 | 多 Wi-Fi 列表（家里 + 手机热点），每个名字试 2 次就换，实测切换 2.5~3.5 秒 |

### 她什么时候会看

| 时机 | 谁触发 | 流量 |
|---|---|---|
| 一直在跑（每 600ms 取一帧丢掉） | 固件 | 0（本地，只为保住自动曝光/白平衡） |
| **每次对话开始**（管理员私聊，上一段已经"聊完了"） | `src/bridge.js` 的 `noteConversationStart()` | 1 张 ≈ 12KB |
| 你说"看看我"、或她自己觉得该看 | 她的工具 `body_photo` | 1 张 ≈ 12KB |
| 常驻推流 | 手动 `CAM STREAM 1` | 2~4fps × 12KB ≈ 40KB/s |

"聊完了没有"用**桥接自己的口径**，不另设一套：`quietGrantMs()`
（默认 **2.5 分钟**，控制台「自由时间」栏可改，钳制 [10s, 60min]）+ 持久化的
`st.lastIncomingAt`。也就是说——他停了 2.5 分钟以上再开口，她就把这当成新的一段对话，
先看一眼再回；中间连续说话不会重复拍。
要单独给摄像头放宽/收紧：环境变量 `QQBRIDGE_CONV_GAP_MIN=<分钟>`。
她**主动**拍不受这个限制——那个节流只管"聊天开头顺手看一眼"。


**还没做**：舵机动作（`motion` 任务那套）、离线模式（没网也点亮屏幕照跑）。

---

## 设计原则（改代码前先看这几条）

1. **意图/轨迹分离**：云端只说 `{"act":"anim","name":"happy","intensity":0.6}`，**永不说角度**。
   限位、缓动、掉电保护全在固件里。
2. **反射不走网络**；失联 5s 回待机、30s 卸力。
3. **只有 `motion` 任务能写舵机**，别的任务发队列消息。
4. **急停是硬件断舵机 VM**，不依赖固件。
5. **群里任何人不能触发动作**；`body_state`（电量/屋里动静）连群聊都不给。
6. **麦克风是 ingress**（屋里声音算资料不算指令），**喇叭是 sink**（不念私聊内容）。

---

## 硬件（e5 这套）

- GOOUUU 果云版 ESP32-S3-DevKitC-1（42 引脚，N16R8）+ 面包板
- INMP441 数字麦克风
- MAX98357 I2S 功放
- 0.96" SSD1306 OLED（128×64，**竖着装**）
- OV2640 摄像头模块（18 脚，**自带 12MHz 有源晶振**）
- 送话键：板载 BOOT（GPIO0）

### 接线

```
INMP441     SCK→GPIO5   WS→GPIO4   SD→GPIO6   VDD→3V3  GND→GND  L/R→GND
MAX98357    BCLK→GPIO15 LRC→GPIO16 DIN→GPIO7  VIN→5V   GND→GND  GAIN→GND
OLED        SDA→GPIO41  SCL→GPIO42  VCC→3V3   GND→GND
按键        BOOT(GPIO0)
```

摄像头（按模块排针 1~18 逐个对，**别按别的板子抄**）：

| 排针 | 名称 | 接到 |
|---|---|---|
| 1 | GND | GND |
| 2 | VCC3.3 | 3V3（模块自己还有一路 5V→3.3 的 LDO，**别往这脚灌 5V**） |
| 3 | SCL | GPIO40 |
| 4 | VSYNC | GPIO38 |
| 5 | SDA | GPIO1 |
| 6 | HREF | GPIO39 |
| 7 | D0 | GPIO8 |
| 8 | RESET（低有效） | **GPIO48** |
| 9~14 | D1~D6 | GPIO9,10,11,12,13,14 |
| 15 | PCLK（丝印也可能叫 DCLK） | GPIO21 |
| 16 | D7 | GPIO17 |
| 17 | PWDN（高有效） | **GND** |
| 18 | NC | 不接 |

三个最容易翻车的点：

1. **INMP441 的 L/R 必须接 GND**。悬空时数据落到右声道，固件只取左声道 ⇒ 串口看着在收，推上去一片静音。
2. **OV2640 的 RESET（排针 8）夹在 D0/D1 之间、PWDN（排针 17）夹在 D7/NC 之间**，位置很反直觉。RESET 悬空会表现为"能读到 PID、但画面是垃圾"或直接 `NO-SOI`。
3. 音频引脚和摄像头引脚**完全不冲突**（音频 4/5/6/7/15/16、屏幕 41/42、摄像头 1/8~14/17/18/21/38/39/40/48）。但 GPIO39/40 是 JTAG 脚，接了摄像头就没法用 JTAG 调试。

---

## 目录

```
firmware/
  d1..d8        最早的九套板子实验（保留作对照）
  e1-breadboard 果云面包板版：只有耳朵、嘴、屏幕
  e2-camera     自己写的 OV7670 工程（没跑通，留作复盘）
  e3-ref        照抄别人的 OV7670 参考实现（验证"别人的代码也一样烂"）
  e4-ov2640     纯摄像头调试工程：串口 / 板载网页 / MQTT 三条推流路径对照
  e5-body-cam   ⭐ 正式版：耳朵 + 嘴 + 屏幕 + 眼睛，一块板
docs/           交接文档：START-HERE（先读）、BODY-BUILD（施工）、BODY-PLAN（战略）、STATUS-LOG（旧账）
tools/          PC 侧：MQTT 音频、Opus、拍照、推流、探活、IDF 包装脚本
```

---

## 编译 / 烧录

需要 **ESP-IDF v5.3.5**。`tools/idf.cmd` 会把 PATH / IDF_PATH / python 环境都设好。

```powershell
# 编译
powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -EA SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\e5-body-cam build'"
# 烧录（这块板是 CH340，端口号可能不是 COM4）
powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -EA SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\e5-body-cam -p COM4 -b 921600 flash'"
```

- ⚠️ **必须先删 `MSYSTEM`**：系统级设了 `MSYSTEM=UCRT64` 时，IDF 的 MSYS 卫兵会直接拒掉，**而且 exit 0、什么都不编**。
- `main/secrets.h` 不进 git。第一次用照 `main/secrets.h.example` 填：Wi-Fi（可以写好几个）、MQTT broker 与账号。
- 串口：CH340（`USB\VID_1A86&PID_7523`）。**e5 的日志波特率是 115200**（e4 是 1500000，别搞混）。

---

## MQTT 协议

板子只跟 broker 说话，跟谁在哪个网段无关。

| 主题 | 方向 | 内容 |
|---|---|---|
| `<base>/audio/up` | 板 → PC | Opus 包（16kHz 单声道） |
| `<base>/audio/down` | PC → 板 | Opus 包，板子解码进喇叭缓冲 |
| `<base>/cmd` | PC → 板 | 文本命令（见下） |
| `<base>/state` | 板 → PC | 每 30s 一条 JSON 心跳（IP/RSSI/heap/麦克风轮次/喇叭包数…） |
| `<base>/cam/jpeg` | 板 → PC | 一帧一张 JPEG |

`<base>` 是主题前缀，每块板不一样（两块板同时在线、前缀相同时 PC 会收到两路，声音忽大忽小、计数乱跳）。

命令（发到 `<base>/cmd`，一条一条发）：

```
MIC 0|1            本地人声门限开/关
MODE indoor|travel 室内 / 随身
PING               回一条 pong（探活用）
CAM SNAP           拍一张，发到 <base>/cam/jpeg
CAM STREAM 0|1     连续推流（默认关，省流量）
CAM RATE <ms>      两帧之间歇多久，0 = 不限
CAM SIZE VGA|QVGA  改分辨率（内部会 deinit + 重新 init）
CAM QUAL 0-63      JPEG 质量，越小越清晰、帧越大
CAM INFO           打印摄像头状态
```

---

## 三条取图路径

同一块板子，三种拿画面的方式，各有各的用处：

| 路径 | 工具 | 实测帧率 | 什么时候用 |
|---|---|---|---|
| 串口 | `tools/cam_stream.py`（e4） | 9.9 fps | 板子就在电脑旁边，要最顺的画面 |
| MQTT | `tools/cam_mqtt.py --body`（e5） | 2~4 fps | 板子在不同网段/带出门，只要能看见 |
| 反向 TCP | `tools/cam_wifi.py`（e4，默认关闭） | 0.3~2 fps | 跨 NAT 抖动太大，已实测更差，留作记录 |

`e4` 还有一条**板载网页**（`http://<板子IP>/`、`/stream`、`/snap.jpg`），只在同网段时能用。

### e5 的串口实时预览（调焦 / 摆机位专用）

`tools/cam_uart_live.py` —— 板子用 USB 插在这台电脑上时用：

```powershell
python -u tools\cam_uart_live.py --port COM4      # 然后开 http://127.0.0.1:8765/
```

实测 **8~10 fps**（一帧 17~22KB，1.5M 波特），延迟 ~0.1 秒，比 MQTT 那条清楚得多，
调焦距、看机位就靠它。结束按 Ctrl-C：脚本会发 `CAM UART 0` 把板子切回 115200 控制台模式。

固件那边就是一条命令 `CAM UART 1|0`（串口或 `<base>/cmd` 都能发），推流期间日志会被关掉，
免得日志字节混进 JPEG。**它和烧录抢 COM4**，推流时不要烧板子。
桥接控制台的「身体 · 眼睛」页也有开始/停止两个按钮（`src/bridge.js` 的
`/api/body/cam-uart/*`），不用记命令。

---

## 已知的坑（都踩过，注释里写得更细）

- **I2C 驱动冲突**：esp32-camera 用老的 `driver/i2c`，OLED 的 esp_lcd 用 driver_ng ⇒ 开机 `CONFLICT!` 直接 panic。解法：`CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK=y`（屏幕 I2C0、SCCB I2C1，两个端口互不相干）。
- **`cam_init` 重名**：esp32-camera 自己就有 `cam_init`，链接期 `multiple definition`。摄像头的对外函数一律 `eye_` 前缀。
- **运行期改分辨率会死**：`set_framesize()` 返回 0，之后取帧一律超时。必须 `esp_camera_deinit()` + 重新 init。
- **2M 波特率 CH340 撑不住**：丢字节、帧头都对不上。1.5M 稳。
- **`NO-SOI - JPEG start marker missing`**：八成是接触问题（RESET 松、排线松），不是软件。
- **Wi-Fi 别在事件任务里阻塞扫描**：`esp_wifi_scan_start()` 放进事件回调会把事件循环堵死，表现为无限重连或日志停在 `enable tsf`。开机就直接连，连不上再扫。
- **OV7670 的教训**（`e2`/`e3` 留着）：QVGA 是 DCW 降采样做的，RGB565 下不可靠；PCLK 太快时 ESP32 采样不到稳定数据（表现为满屏雪花，降 `CLKRC` 到 XCLK/3 立刻干净）；内置彩条正常 ≠ 真实画面正常（彩条绕开镜头和 ADC）。
- **别用 `printf`/`fwrite(stdout)` 发二进制**：stdio 是文本模式，JPEG 里每个 `0x0A` 会扩成 `0x0D 0x0A`，一帧凭空多出上百字节，主机按 `META` 里的长度切出来永远找不到 `FFD9`（实测 declared 17584 / 实际 17689）。要发裸数据就用 `uart_write_bytes()`。
- **切帧别数错帧头的长度**：`"META,JPEG,"` 是 10 个字符，不是 8。数错的话每帧都 `ValueError` 被当坏帧丢掉，现象是"板子好像没出图"，其实是解析器空转。

---

## 状态

个人项目，在用的就是 `e5-body-cam`。`d1`~`d4` 是早期的分层实验，`e2`/`e3` 是没跑通的弯路，都留着当参考，别指望它们能直接烧。

PC 侧（`tools/`）默认连的是作者家里那台 EMQX，**你复刻的话要自己有一个 MQTT broker**，把 `secrets.h` 和 `tools/body_config.py` 指向它。

---

## 许可证

暂未选择许可证。想拿去用、改、抄，直接说一声就行。
