# e1-breadboard —— 果云版 42 脚开发板 + 面包板的「耳朵和嘴」

第三套固件。硬件是 **GOOUUU 果云版 ESP32-S3-DevKitC-1（42 引脚）+ 面包板**
接 INMP441 + MAX98357，接线照小智 AI 面包板教程 2.2 节。

能力与 `d8-mqtt-audio` 完全一致（MQTT 上下行音频、本地人声门限、室内/随身两种模式、
Opus 上行、8 秒喇叭缓冲），**唯一的差别是引脚和主题前缀**。

> ⚠️ 这套是 `d8-mqtt-audio` 的复制。改了音频/MQTT 逻辑，**两个工程都要改**；
> 只想改一处的话，就别维护两份 —— 那说明该回到 d8 里做板型开关。

---

## 1. 接线（和 d8 那块语音板不一样，别照抄）

```
INMP441    SCK→GPIO5   WS→GPIO4   SD→GPIO6   VDD→3V3   GND→GND   L/R→GND
MAX98357   BCLK→GPIO15 LRC→GPIO16 DIN→GPIO7  VIN→5V    GND→GND   GAIN→GND
按键       BOOT（GPIO0）→ 一按就开麦          （可选：别的空闲脚，改 PIN_BTN）
```

拿 d8 那块 wangxun 语音板对照：

| 信号 | wangxun 语音板 | 这块面包板 |
|---|---|---|
| 麦克风 BCLK / WS | 4 / 5 | **5 / 4** ← 交叉 |
| 麦克风 SD | 6 | 6 |
| 功放 BCLK / LRC / DIN | 16 / 17 / 15 | **15 / 16 / 7** |
| 送话按键 | IO1 | **GPIO0 (BOOT)** |

**接反不是"声音小"，是彻底没声音，而串口日志一切正常。** 别靠听去猜。

三个最容易翻车的点：

1. **INMP441 的 L/R 必须接 GND。** 悬空时数据可能落到右声道，而固件只取左声道
   ⇒ 串口看着在收，推上去是一整片静音。
2. **3V3 / GND 排针不够分**（麦克风、功放、按键都要）。买几条 1 分 2 的分叉杜邦线，
   或者把面包板的电源轨拉出来用。
3. **BGA 封装那种 MAX98357 不要接 GAIN**（接了能正常出声就继续用）。

## 2. 和 d8 同时在线要注意主题前缀

| 板子 | 工程 | `TOPIC_BASE` |
|---|---|---|
| wangxun 语音板 | `d8-mqtt-audio` | `whale-1bb62897` |
| 果云面包板 | `e1-breadboard` | `whale-8f2a5c31` |

两条板子同时上电、前缀又一样时，PC 会同时收到两块板的 state 和音频
（表现：声音忽大忽小、计数器乱跳、日志里 IP 一会儿这个一会儿那个）。
**换前缀要同步改三处**：本工程 `main/config.h` 的 `TOPIC_BASE`、
控制台「身体 · 连接」里的主题前缀、以及 `tools/body_config.py` 的默认值（可选）。

## 3. 编译 / 烧录

⚠️ 这台机器系统级设了 `MSYSTEM=UCRT64`，IDF 的 MSYS 卫兵会直接拒掉、**还 exit 0 什么都不编**。
必须先在 PowerShell 里删掉这个环境变量：

```powershell
powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\e1-breadboard build'"
powershell -NoProfile -Command "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; cmd /c 'tools\idf.cmd -C firmware\e1-breadboard -p COM4 flash monitor'"
```

**这块板是 CH340 串口，不是 d8 那种原生 USB 口**：插上后设备管理器里会出现
`USB-SERIAL CH340 (COM4)`（`USB\VID_1A86&PID_7523`）。第一次插 Windows 会先报一个
`CM_PROB_FAILED_INSTALL`（黄色感叹号、没有 COM 号），**等几秒它自己就装好了**（驱动 3.9.2024.9），
别急着去下驱动。端口号会随插拔变，烧之前先扫一次：

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name
```

`main/secrets.h` 不进 git（见 `.gitignore`），第一次用照 `secrets.h.example` 填 Wi-Fi 和 MQTT 账号。

## 4. 第一次上电怎么验

1. 串口看有没有 I2S 报错、`mode`/`rssi` 有没有打印出来。
2. `state` 心跳里 `mic` 默认是 0（门限关着），发 `MIC 1` 才开。
3. **验线**：PC 侧把控制台主题前缀改成 `whale-8f2a5c31`，然后

   ```bash
   python tools/body_probe.py --timeout 12      # 能拿到 state 就说明连上了
   python tools/body_talk.py --text "测试一下"   # 她回一句，喇叭念出来
   ```

   说话时看上行 `rms` 是否明显高于底噪（安静房间底噪 rms 个位数到 20 几，
   说话 100~500）。**rms 不动 = 麦克风的 SCK/WS 接反了或 L/R 没接地**，
   不用怀疑代码 —— 同样的代码在 d8 那块板上是通的。
4. 喇叭不出声：先确认 `MAX98357` 的 `SD` 悬空（常开）、`GAIN` 接 GND，
   再确认功放那三根（15/16/7）。

## 5. 硬件前提

- **ESP32-S3 N16R8**（16 MB Flash + 8 MB **八线 OCT** PSRAM）。`sdkconfig.defaults`
  里 `CONFIG_SPIRAM_MODE_OCT=y`，8 MB PSRAM 用来放 256 KB 喇叭缓冲和 Opus 编解码任务的栈。
- 买到 N8R2（2 MB 四线 PSRAM）也能启动，但缓冲会退回 64 KB、Opus 任务可能起不来 ——
  那种情况要改 `sdkconfig.defaults` 并回退上行编码。
