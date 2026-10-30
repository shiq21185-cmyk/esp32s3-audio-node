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
2026-10-30  ✅ 固件支持第二块板：果云版 42 引脚开发板 + 面包板（一块固件两块板）
            用户原话（m00002）：「我额外按照虾哥的小智ai链接方式做了一版面包板的硬件设备，
              https://my.feishu.cn/wiki/EH6wwrgvNiU7aykr7HgclP09nCh 里面的果云版本」。
            · 教程里有两套接线图，**必须选 2.2「GOOUUU 果云版 42 IO 引脚」**（还有 2.3.2 是带 4G 的），
              别拿 2.1 标准版 44 引脚那张 —— 两张图的 GPIO 不一样。
              我直接把那张接线表的标号放大读出来了（表格是图片，正文里读不到）：
                INMP441  : SCK→GPIO5  WS→GPIO4  SD→GPIO6
                MAX98357 : BCLK→GPIO15 LRC→GPIO16 DIN→GPIO7   GAIN→GND
                OLED(可选): SDA→GPIO41 SCL→GPIO42   按键: 音量+ GPIO40 / 音量- GPIO39 / BOOT GPIO0
            · 和现有 wangxun 语音板比：麦克风的 BCLK/WS **是交叉的**（4/5 ↔ 5/4），
              功放三根全不一样（16/17/15 → 15/16/7）。接反不是声音小，是彻底没声，
              而串口日志一切正常 —— 极易误判成"固件坏了"。
            · 改法：`firmware/d8-mqtt-audio/main/config.h` 顶部加 `BOARD_PROFILE` 开关，
              `BOARD_WANGXUN`（默认，现有实物）/ `BOARD_GOOUUU`（果云面包板）。
              换板子只改这一个宏，再 build + flash；`#else` 分支是 `#error`，写错板型编不过。
              果云板没有"录音测试"键，送话键用板上自带的 BOOT（GPIO0）；
              副作用是**开机时按住会进下载模式**，正常使用无影响，嫌碍事改 `PIN_BTN` 即可。
            · 面包板侧的坑（写进 docs/BODY-BUILD.md §3.2.2）：
              INMP441 的 **L/R 必须接 GND**（悬空 ⇒ 数据落右声道 ⇒ 固件只取左声道 ⇒ 全静音）；
              果云板 3V3/GND 排针不够分，教程建议 1 分 2 分叉杜邦线；
              教程的 BGA 版 MAX98357 不要接 GAIN。
            · 验证：`BOARD_WANGXUN` 与 `BOARD_GOOUUU` 两种板型各完整 build 一次，
              均 1034/1034 通过、`d8_mqtt_audio.bin` 0x11c940 字节（分区剩 24%）；默认已还原 wangxun。
              ⚠️ 编译要用 powershell 并**先删掉 MSYSTEM**（这台机器系统级设了 MSYSTEM=UCRT64，
                 IDF 的 MSYS 卫兵会直接拒掉、还 exit 0 什么都不编）：
                 powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -EA SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\d8-mqtt-audio build'"
              ⚠️ **还没在果云实物上通电验证**（板子刚焊/刚插好，不知道用户手里是不是这套）。
                 第一次上电先看串口有没有 `I2S` 报错，再 `body_talk.py --text` 看上行 rms。


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
