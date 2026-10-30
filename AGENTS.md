# AGENTS.md · 小鲸鱼的身体

> 这个文件会被 DSH **自动加载**到每个新会话的第一条消息里。
> 你（未来的 agent）不用等人解释背景 —— 读完这三节就能直接干活。

## 1. 这个项目是干什么的

给 **小鲸鱼**（一个跑在 `C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy` 里的软件人格，
通过 QQ 说话，脑子是 DSH agent）加一个**物理身体**：
ESP32-S3 + FreeRTOS + TTL 总线舵机 + 屏幕眼睛 + 麦克风/喇叭。

**智力留在 PC，板子只做两件事**：本地反射（<50ms，不走网络）+ 表现层（把"开心"演成动作）。

主人背景：熟 STM32 / FreeRTOS / 嵌入式；**没做过 ESP32**；不需要画 PCB，全用现成模块 + 3D 打印。

## 2. 先读这三份（顺序固定）

| 文件 | 什么时候读 |
|---|---|
| `docs/START-HERE.md` | **永远第一个读**。交接摘要 + 已定死的决定 + 血泪清单 + 待办 |
| `docs/BODY-BUILD.md` | 施工时读。采购/电流/引脚/接线/协议/环境/代码骨架/联调顺序 |
| `docs/BODY-PLAN.md` | 想推翻方案前读。战略篇，讲为什么这么分 |

**不要再从零设计。** 方案已经定稿，改动请先说明理由再动手。

## 3. 硬约束（违反就是错，不要"优化"掉）

1. **意图/轨迹分离**：云端只说 `{"act":"anim","name":"happy","intensity":0.6}`，**永不说角度**。
   限位、缓动、掉电保护全在固件。
2. **反射不走网络**；**失联 5s 回待机、30s 卸力**。
3. **只有 `motion` 任务能写舵机**，别的任务发队列消息。
4. **急停是硬件断舵机 VM**，不依赖固件。
5. **群里任何人不能触发动作**；`body_state`（电量/屋里动静）连群聊都不给。
6. **麦克风是 ingress**（屋里声音算资料不算指令），**喇叭是 sink**（不念私聊内容）。

## 4. 当前状态（每次改完请更新这段）

```
2026-10-30  ✅ 第三套固件：果云版 42 脚 + 面包板独立工程 `firmware/e1-breadboard`
            用户原话（m00002）：「我额外按照虾哥的小智ai链接方式做了一版面包板的硬件设备，
              https://my.feishu.cn/wiki/EH6wwrgvNiU7aykr7HgclP09nCh 里面的果云版本」；
            澄清（m00547）：「我是需要依照果云的做一个新的面包板类型的，不再是本地已经存在的
              这个项目，新的还没写代码」⇒ **第三套是独立工程**，不是往 d8 里塞板型开关。
            · 教程有两套接线图，**必须选 2.2「GOOUUU 果云版 42 IO 引脚」**（2.3.x 是带 4G 的），
              别拿 2.1 标准版 44 引脚那张。接线表是**图片**，正文读不到 ——
              我把图放大 4 倍逐根读出来（PIL 裁 .tmp/crop/*.png）：
                INMP441  : SCK→GPIO5  WS→GPIO4  SD→GPIO6  VDD→3V3  L/R→GND
                MAX98357 : BCLK→GPIO15 LRC→GPIO16 DIN→GPIO7  VIN→5V  GAIN→GND
                OLED(可选): SDA→GPIO41 SCL→GPIO42   按键: 音量+ 40 / 音量- 39 / BOOT 0
            · 和 wangxun 语音板比：麦克风 BCLK/WS **交叉**（4/5 ↔ 5/4），功放三根全换
              （16/17/15 → 15/16/7）。接反不是声音小，是彻底没声，而串口日志一切正常。
            · 工程：`firmware/e1-breadboard`（从 d8 复制，CMake 工程名 e1_breadboard，
              日志 TAG 改成 e1，能力与 d8 完全一致）。引脚直接写进它的 `main/config.h`，
              **不再有 BOARD_PROFILE** —— d8 已恢复单一板型。
              送话键用板载 BOOT（GPIO0）；副作用是**开机时按住会进下载模式**，正常用无影响。
            · 主题前缀必须隔离：d8 是 `whale-1bb62897`，e1 是 **`whale-8f2a5c31`**。
              两条板同时在线、前缀一样时 PC 会同时收到两路（声音忽大忽小、计数器乱跳）。
              换前缀三处：本工程 config.h / 控制台「身体·连接」/ body_config.py 默认值（可选）。
            · 面包板侧的坑（docs/BODY-BUILD.md §3.2.2）：INMP441 的 **L/R 必须接 GND**
              （悬空 ⇒ 数据落右声道 ⇒ 固件只取左声道 ⇒ 全静音）；3V3/GND 排针不够分，
              建议 1 分 2 分叉杜邦线；BGA 版 MAX98357 不接 GAIN。
            · 验证：e1 完整 build 1034/1034 通过，`e1_breadboard.bin` 0x11c940（分区剩 24%）。
              ⚠️ 编译必须 powershell + **先删 MSYSTEM**（系统级 MSYSTEM=UCRT64，IDF 的 MSYS 卫兵
                 会直接拒掉、**还 exit 0 什么都不编**）：
                 powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -EA SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\e1-breadboard build'"
            · ✅ **实物通了（2026-10-30 16:0x）**：插上后枚举成 **USB-SERIAL CH340 (COM4)**
              （USB\VID_1A86&PID_7523）—— 这块面包板不是 d8 那种原生 USB 口，
              第一次插 Windows 会先报 CM_PROB_FAILED_INSTALL，**等几秒自己装好**，
              别急着装驱动；驱动版本 3.9.2024.9。
              烧录 OK，启动日志：Wi-Fi TP-LINK_5D90 → IP **192.168.1.107**，
              TLS 连上 EMQX，前缀 whale-8f2a5c31，TAG `e1`，
              自检打印「麦克风 5/4/6，功放 15/16/7，16000 Hz」。
              喇叭：`mqtt_audio.py --base whale-8f2a5c31 tone --seconds 1.5` → 25 包、
              ddrop 0、(spkb-spks)/64 = **1680ms 真语音** ⇒ 功放那段通了（用户听感待确认）。
              麦克风：`.tmp/e1_mic_check.py 8` → 8 秒 136 包（17/s = 60ms/包 ✓），
              rms 中位 14、最大 1440、peak 18011 ⇒ INMP441 接线对（不是全 0）。
              ⚠️ 工具坑：`mqtt_audio.py` 的 `--base` 是**顶层参数**，要写在子命令前面
              （`mqtt_audio.py --base X tone`，写成 `tone --base X` 会 unrecognized）。
              ⚠️ 直接 `body_mqtt.Link(base=...)` 会 `Bad user name or password` ——
              账号密码在控制台配置里，必须用 `body_config.link_kwargs()` 再覆盖 base。
            · 🔴 **第一次跑整轮失败，根因不在板子：她的 ASR 起不来。**
              报 `ASR 模型加载失败：加载模型失败：Invalid port: ':1]'`。
              根因链：模型本地已有、`HF_HUB_OFFLINE=1` 也设了，但
              `WhisperModel("small", download_root=...)` 仍会**先建一个 HTTP client**；
              它读 Windows 系统代理的例外表（FlClash 写进 ProxyOverride 的 `[::1]`），
              httpx 把 `[::1]` 当 URL 解析 ⇒ `InvalidURL: Invalid port: ':1]'`。
              ⇒ 系统代理一开，她整条语音链路（含 QQ 语音转写）就哑，而日志只说"模型加载失败"。
              修（改的是她本体 `qq-bridge-deploy/tools/asr/` 两个文件，非本仓库）：
                transcribe.py 新增 `local_snapshot()`：本地有快照就返回**目录路径**，绕开 hub；
                worker.py 先解析本地快照再 `WhisperModel(resolved, ...)`，多打印一行「用本地快照：…」。
              实测同一份缓存：传 "small" 报错，传目录 955~1186ms 就绪。
              ⚠️ 若 QQ 侧语音仍哑，重启桥接（`POST /api/restart`）让旧的常驻 worker 换掉。
            · ✅ **端到端整轮通了（2026-10-30 傍晚，e1 面包板）**：
              他说「喂?现在可以说话吗?」→ 听写 2384ms；她想了 3373ms 回「能听见，正听着呢。你想说啥？」；
              下行 33 包 / 3960ms 音频 / **缺料 0** / 最低水位 2395ms / 掉线 0；
              板子侧写 I2S 4020ms（其中 2400ms 是垫底静音）、溢出 0、残留 buff 2280ms；
              首音 969ms、念完 4115ms、本轮 7488ms。
              ⇒ 第三套（果云面包板）的耳朵、嘴、MQTT、TTS、她的大脑，全链路成立。
            · 还没做：① 控制台「身体·连接」的主题前缀还是 d8 那个（whale-1bb62897），
              要跟 e1 说话得手动切，或者定一块板当默认；② QQ→板子的 `body_say`
              还没在 e1 上验过（d8 上验过）；③ e1 的喇叭听感（会不会发闷/发卡）
              数据上没毛病，还没让他逐句确认。
            · 公开仓库 esp32s3-audio-node 另有一份「通用版」（板型开关 BOARD_INTEGRATED /
              BOARD_BREADBOARD，commit 1ff1f79 已推），那是给外人看的版本，和这里互不干扰。
            · ✅ **屏幕也点上了（2026-10-30 傍晚）**：先按 0.91" 128x32 做的，
              主人随后换成市面最常见的 **0.96" 128x64**，而且**竖着装的**
              （排针在短边）⇒ 逻辑画布变成 **64 宽 × 128 高**，界面是"一个瘦瘦的小人"。
              驱动不抄小智的 LVGL（那是 lv_obj 树 + 中文位图字，几百 KB），
              用 IDF 自带的 `esp_lcd_panel_ssd1306` + 一帧 1bpp 缓冲（1KB）
              + 自己的画点/写字（`oled.c` / `ui.c` / `font5x7.c`）。
              排版开关是 **`config.h` 的 `OLED_ROT`**（0~7：低两位旋转 0/90/180/270，
              +4 = 再左右翻；SSD1306 没有旋转寄存器，90° 是软件转置）。
              现在取值 **1**（竖装）。四个值试一遍一分钟，1/3/5/7 都是竖的。
              画的人永远按 `OLED_W x OLED_H`（竖装时 64x128）想问题，
              `oled_pixel()` 里做一次 `logical_to_panel()`，flush 不再转换。
              ⚠️ 硬件镜像固定关掉：硬件翻一次 + 软件翻一次会互相抵消，调起来像掷骰子。
              ⚠️ `OLED_ENABLE 0` 时 oled.c 整份退化成空实现，没接屏的板子照跑。
            · 🔴 屏幕踩的两个坑：
              ① **板子每秒重启一次**，日志停在 `wifi_init` 那行，看着像 Wi-Fi 的错；
                 真身是 `assert failed: xStreamBufferBytesAvailable (pxStreamBuffer)`——
                 UI 在 app_main 早期就起来了，而 `spk_is_busy()` 里的 `s_spk`
                 要等 `audio_init()` 才建，传 NULL 直接断言。已加 NULL 兜底。
              ② `main.c:994` 原本是 `MQTT_USER[0] != '\\0'`（双反斜杠 = 多字符常量，
                 **恒为真**），所以"空账号就不填"那条分支从来没生效过。已改成 `'\0'`。
            · 🎨 1:1 排版只能用 ASCII 自查（当前模型读不了图片）：
              `.tmp/figure_lab.py` 打印 `#`/`.` 的人形，`.tmp/ui_portrait.py`
              打印整屏并输出 `.tmp/ui_portrait.png` 给主人看。
              试出来没用的几版（别再走回头路）：空心方框眼像眼镜片；
              实心块拼一起糊成一坨；手臂贴躯干边线会让轮廓变成 3px 黑边
              （必须从 `cx±10` 起笔，肩膀处躯干半宽 7，留 2px 缝）。
              定稿：状态栏 0..8 / 头 `head_y=28,r=12` / 脖子 / 躯干 42..74
              / 腿 75..95 / 地面 96 / 电平条 110..117。
            · 还没做：① 主人要确认实物**字是不是正的**（不正就调 `OLED_ROT`）；
              ② 128x64 竖装下 64px 宽塞不下 IP，状态栏只能打单字母 `O/I/L/S`；
              ③ 控制台「身体·连接」前缀仍是 d8 的 whale-1bb62897；
              ④ `body_say` 还没在 e1 上验；⑤ e1 喇叭听感待确认。


2026-10-24 ~ 2026-10-28 的全部逐日记录（D1~D8、Opus、喇叭缓冲、state 截断、
opus 编码器 drain、QQ↔身体、记忆双档……所有踩坑与验证数字）已搬到
`docs/STATUS-LOG.md`。要查旧账就去那里面 grep。
```
## 5. 两个仓库的关系

- **本工作区 `rot`**：身体（固件 + 文档 + PC 侧新增代码草稿）。
- **`..\QQ bot\qq-bridge-deploy`**：她现有的软件本体（bridge / 人格 / MCP）。
  最终 `src/mcp-body.js` 要落进那边，因为 DSH 是从那儿 spawn MCP 的。
  ⚠️ 那个目录的 ACL 有问题（`Everyone:(DENY)`），普通权限下 bash 起不来 ——
  在那边干活需要 danger-full-access。**本工作区没有这个问题。**

## 6. 工作方式

- 中文回答。改动前先读文件；改完跑测试。
- 硬件相关数字（电压、电流、限位）**不要编** —— 标注"待实测"，并写清怎么测。
- 主人会自己焊接、自己量。给他**能照着做的步骤**，不是概念。
