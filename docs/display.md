# 屏幕：一块 0.96" I2C OLED 上的小表情

> 一句话：**屏幕是可选件**，接不接都能跑；接上之后，屋里的人扫一眼就知道
> 板子在线吗、对端在听吗、对端在说吗、声音有多大。

---

## 1. 为什么不用 LVGL

小智那套（以及大多数 ESP32 语音玩具）用 LVGL 画屏：一套 GUI 框架 + 中文字库，
一进来就是几百 KB flash 和几 MB PSRAM，还要拉一堆 lv_obj 生命周期。

这块屏只显示**几十个字符 + 一张几十像素的小人**。为一个 64×128 的单色屏拖一套 GUI，
不划算 —— 而且这套固件是纯 C、单文件风格，加个依赖会让整机变得难改。

所以这里的做法是：

```
I2C 总线 + IDF 自带的 esp_lcd_panel_ssd1306 驱动
        + 一帧 1bpp 帧缓冲（128×64 只有 1 KB）
        + 二十来个画点/画框/写字的函数（oled.c）
        + 一个 10 fps 的绘制任务（ui.c）
```

没有第三方库，除了 IDF 自带的 `esp_lcd`。

---

## 2. 接线与配置

| 信号 | 接到 | 说明 |
|---|---|---|
| VCC | 3.3 V | SSD1306 模块板上有稳压/电平转换的可以接 5 V，但 3.3 V 最稳 |
| GND | GND | |
| SDA | **GPIO41** | `config.h` 的 `PIN_OLED_SDA` |
| SCL | **GPIO42** | `config.h` 的 `PIN_OLED_SCL` |

地址固定 `0x3C`（`OLED_ADDR`）。I2C 速率 400 kHz。

`config.h` 里三个必须填对的：

```c
#define OLED_ENABLE   1        // 0 = 整个屏幕代码退化成空实现，不占 flash
#define OLED_HEIGHT   64       // 32 = 0.91"，64 = 0.96"
#define OLED_ROT      0        // 朝向，0~7；竖装用 1 或 3（见下）
```

### ⚠️ `OLED_HEIGHT` 填错不会报错

SSD1306 **不能回读自己的高度**。填 32 而实物是 64 ⇒ 下半屏空着；
填 64 而实物是 32 ⇒ 只看到上半截，而那半截本身是完整 UI。

### ⚠️ 竖装 / 横装靠 `OLED_ROT` 一个数字

| 值 | 逻辑画布 | 适用 |
|---|---|---|
| 0 | 128×64 | 排针在长边（横装） |
| 1 | 64×128 | 排针在短边（竖装，逆时针 90°） |
| 3 | 64×128 | 竖装，顺时针 90° |
| 4/5/6/7 | 同上 + 左右翻转 | 镜像用的 |

SSD1306 没有旋转寄存器，90° 是**软件转置**：`oled_pixel()` 里把逻辑坐标映射到面板坐标，
所以画的人永远按 `OLED_W × OLED_H` 这块画布想问题，`oled_flush()` 不做第二次转换。

装错方向时：字是歪的/人是横躺的，把 `OLED_ROT` 换成 1 或 3 试一次就知道（四种组合一分钟试完）。

---

## 3. 界面长什么样

### 竖装（64×128）—— 一张小人 + 底部电平条

```
   ┌────────┐
   │  呆毛   │
   │ ┌────┐ │   ← 描线点阵小人（sprite.h）
   │ │ 脸 │ │
   │ └────┘ │
   │  身体   │
   │  裙子   │
   │  腿     │
   │ ─────  │   ← 地面线（画在点阵里）
   │ ▓▓░░░  │   ← 电平条（跟着 rms 走）
   └────────┘
```

只有两处会动：

- **嘴**：`ST_SPEAK` 时把底图那颗 2×2 的小嘴补成方块（只多一个像素）；
- **尾巴**：`ui.c` 的 `TAIL_WAG`，置 1 才晃（绕 (47,72) 转 ±10°、1.6 s 来回）。
  出厂为 0，因为静态画面更省电、也不容易看出点阵的锯齿。

四个状态由 `pick_state()` 决定：`OFFLINE`（没连上 broker）/ `IDLE` / `LISTEN`（门限开着或
按键窗口内）/ `SPEAK`（喇叭有料）。

### 横装（128×64）—— 一张宽脸 + 状态栏 + 数字

顶上一道实心白条（反色字）：模式 `G`/`B`、Wi-Fi 四柱、MQTT 方块、麦克风图标、右侧 IP。
中间是一对眼睛 + 嘴，底下是电平条，再往下是 `UP/DN`、`IP`、`MIC/GATE`、状态名。

横装这套是给"屏幕够大"的场合准备的；竖装那块 64 px 宽塞不下 IP，所以只画小人。

---

## 4. 换掉那张小人（`sprite.h`）

`sprite.h` 里是两张 1bpp 点阵：`k_body[128][8]` 和 `k_tail[16][2]`（尾巴单独存，方便做摆动）。
每行 8 字节，`bit0` = 该行最小 x。

**换成你自己的形象**，任何 64×128 的黑白 PNG 都能转：

```python
from PIL import Image

# 1) 取"身形"：有 alpha 用 alpha，没有就用阈值
im = Image.open("me.png").convert("RGBA")
mask = im.split()[3].point(lambda v: 255 if v > 128 else 0)
#     （或用 im.convert("L").point(lambda v: 255 if v < 128 else 0) 取深色线条）

mask = mask.resize((64, 128), Image.BOX)          # BOX = 块平均，细线不会消失
b = mask.point(lambda v: 255 if v > 110 else 0).convert("1")

# 2) 按行打包
px = b.load()
for y in range(128):
    row = [0] * 8
    for x in range(64):
        if px[x, y]:
            row[x // 8] |= 1 << (x % 8)
    print("    {%s},  /* y=%d */" % (",".join("0x%02x" % v for v in row), y))
```

### 描线的两条经验（都踩过）

1. **先在高分辨率下取"线"，再缩到 64×128。** 一张 1024 宽的线稿直接缩到 64 px，
   8 px 宽的线会薄成 0.5 px，整片消失 ⇒ 脸变成黑洞。
   正确做法：`alpha 取身形` + `亮度取黑线`，白身体挖掉黑线，得到"贴纸"效果。
2. **别指望在 64 px 上画"深色头发"。** 1bpp 只有黑白两档，头发和脸都是白的时候，
   任何分隔线都会被读成帽檐/边框/眼镜。要么把发型做进**外轮廓**（长发作成轮廓的一部分），
   要么用 50% 棋盘抖动当第三档（屏幕上能读出"灰"，但边缘会锯齿）。
3. **没有 alpha 通道的图就直接二值化**，别去描线 —— 描线那套对纯黑底白线的图才有效。

---

## 5. 掉线与状态

`ui_status_t` 是 `main.c` 每 100 ms 填一次的快照（`ui_snapshot()`）：

| 字段 | 来源 |
|---|---|
| `wifi_ok` / `ip` | `IP_EVENT_STA_GOT_IP` 置位（`s_wifi_ok`），IP 现取 `esp_netif_get_ip_info` |
| `mqtt_ok` | MQTT 连接状态 |
| `mic_on` / `gate_open` / `btn_window` | 板子的门限与按键窗口 |
| `speaking` | `spk_is_busy()`（喇叭缓冲有料 = 正在放） |
| `rms` / `floor` | 最近一包音量 / 房间底噪估计 |

⚠️ **UI 早于 `audio_init()` 就会起**（屏幕在 Wi-Fi 连上之前就点亮）。
所以 `spk_is_busy()` 必须判空 `s_spk`，否则第一次取快照就撞
`assert failed: xStreamBufferBytesAvailable (pxStreamBuffer)`，表现是板子每秒重启一次 ——
而串口日志停在 `wifi_init` 那行，看起来完全像 Wi-Fi 的错。

---

## 6. 屏幕没接 / 接错会怎样

`oled_init()` 的顺序是：

1. `i2c_new_master_bus()`
2. **`i2c_master_probe(bus, 0x3C, 100)`** —— 先探一次地址，"没接屏"时能直接说清楚
3. `esp_lcd_new_panel_io_i2c()` → `esp_lcd_new_panel_ssd1306()` → `reset/init/mirror/disp_on`

任何一步失败都只打一行 `ESP_LOGW` 然后 `return false`，`ui_start()` 就不启动。
**音频和 MQTT 完全不受影响** —— 别为了屏幕把整机搞得起不来。
