# MQTT 线协议（firmware/audio-node-mqtt）

> 一句话：**四个话题，两种包类型**。板子和电脑都只做出站连接，谁在 NAT 后面都无所谓。

UDP 版（[protocol.md](protocol.md)）要求两端在同一张网里。这一版把声音搬上 MQTT：
只要两边都能连上 broker，板子挂在 4G、别人家的 Wi-Fi、还是你的手机热点，都无所谓。

```
   板子 ──publish──► <base>/audio/up     ──┐
        ──publish──► <base>/state          │
                                            ├──►  broker  ──►  电脑
   板子 ◄──subscribe─ <base>/audio/down  ──┤
        ◄──subscribe─ <base>/cmd           ─┘
```

---

## 1. 话题

`base` 就是固件 `config.h` 里的 `TOPIC_BASE`，两端必须一模一样。

| 话题 | 方向 | 内容 |
|---|---|---|
| `<base>/audio/up` | 板子 → 电脑 | 麦克风音频，见 §3 |
| `<base>/audio/down` | 电脑 → 板子 | 喇叭音频，见 §3 |
| `<base>/state` | 板子 → 电脑 | JSON 状态，见 §5 |
| `<base>/cmd` | 电脑 → 板子 | ASCII 指令，见 §4 |

全部 **QoS 0**（最多一次）。语音流重传没有意义 —— 迟到的包不如不要。

### 选 base 的两个要求

1. **随机**，不要用能推导的值（MAC 后三字节之类）。公共 broker 上任何人猜出你的
   前缀和 client id，就能把你的板子顶下线（MQTT 同 id 后到者踢前者）。
   ```bash
   python -c "import secrets;print('node-'+secrets.token_hex(4))"
   ```
2. **两端一致**：固件 `config.h` 的 `TOPIC_BASE`，客户端 `--base`。

客户端调 `--base` 忘了改是「ping 不通」的头号原因：链路上什么都没错，只是两边在说不同的话题。

---

## 2. 音频格式

| 项 | 值 |
|---|---|
| 采样率 | 16000 Hz |
| 位深 | 16 bit 小端（s16le） |
| 声道 | **1（单声道）** |
| 一帧 Opus | 960 采样 = 60 ms |
| 裸 PCM 一包 | 1920 采样 = 3840 字节 = **120 ms** |
| 码率（Opus） | 16 kbit/s |
| 码率（裸 PCM） | 256 kbit/s |

**为什么线上是单声道**：麦克风只有一路有效声音（INMP441 的 L/R 接地时数据在左声道），
功放也只推一只喇叭。传立体声等于带宽翻倍换不来任何东西。板子内部 I2S 仍然是立体声
（I2S 需要时钟），只在发出去之前抽左声道。

**上行为什么是 Opus**：裸 PCM 是 32 KB/s，一小时 115 MB，出门用几小时就把配额吃穿。
Opus 16 kbit/s 是 2 KB/s，同样配额能讲一百多小时。

**下行为什么是裸 PCM**：Opus 解码要在板子上跑，PC 边合成边送时板子的解码容易跟不上，
听感变成「前面正常、后面一卡一卡」。裸 PCM 多花带宽，但播放路径上零计算 —— 值。

---

## 3. 音频包格式

每个包的**第一个字节是包类型**。

| 首字节 | 名字 | 后面是什么 | 谁发 |
|---|---|---|---|
| `0xAA` | 裸 PCM | 3840 字节 s16le 单声道 | 下行；上行兜底 |
| `0xAB` | 单帧 Opus | 一个完整 Opus 帧（约 120 字节） | 旧格式，板子还认 |
| `0xAC` | Opus 打包 | 循环的 `[len_lo len_hi][Opus 帧]` | 上行（当前默认） |

### 0xAC 打包格式

Opus 一帧只有 120 字节，60 ms 一帧就是每秒 16.7 个包。很多托管 broker 按
**每秒消息条数**限流（实测 Serverless 档位就是如此），16.7 包/秒会掉到 55~60% 投递率。
把 3 帧合成一个包（5.6 包/秒）后投递率 100%，代价是单向多 180 ms 延迟。

```
偏移  内容
0     0xAC
1     len_lo          ← 第 1 帧长度，小端 16 位
2     len_hi
3..   第 1 帧 Opus 数据
...   len_lo len_hi 第 2 帧
...   len_lo len_hi 第 3 帧
```

读到包尾为止，**不写死帧数** —— 将来改帧长/帧数两端还能兼容。解析时遇到残缺的尾巴
直接丢掉（丢掉一帧 60 ms，比整包报废好）。

### 上行兜底

板子编码失败时会直接发 `0xAA` + 裸 PCM。所以客户端必须同时会处理三种包，
不能假设上行永远是 `0xAC`。

---

## 4. 指令（`<base>/cmd`）

ASCII 文本，**整串精确比较**（板子会先拷前 127 字节）。所以 `MIC 1` 可以，`mic 1` 不行。

| 指令 | 作用 |
|---|---|
| `PING` | 立刻回一条 state（`"kind":"pong"`）。**探活不要等心跳，用这个** |
| `MIC 1` | 开麦克风 |
| `MIC 0` | 关麦克风 |
| `GATE 1` / `GATE 0` | 开/关本地人声门限 |
| `MODE 0` | 室内模式：人声门限 + 回声保护（默认） |
| `MODE 1` | 随身模式：按键开一个 20 秒送话窗口，不靠门限 |
| `ECHO 1` / `ECHO 0` | 开/关回声保护（`ECHO 0` 只在调试打断时用） |

板子**开机时麦克风是关的**。不显式发 `MIC 1` 就永远收不到音频 —— 这是有意的：
不该一上电就往公网推屋里声音。

`MODE` 会存进 NVS，掉电保持。

---

## 5. 状态（`<base>/state`）

JSON 文本，一行。空闲时每 `STATE_PERIOD_MS`（默认 30 秒）一条，`PING` 时立刻一条。

```json
{"kind":"pong","up":1201,"rssi":-58,"heap":7857896,
 "apkts":114,"dpkts":455,"dbytes":873600,"ddrop":0,"sbuf":0,
 "mic":1,"gate":0,"rms":6,"floor":25,"mode":0,
 "enc":337,"encb":40440,"encf":0,
 "mloops":9900,"mreads":9800,"mpub":120,
 "skoff":12,"skgate":800,"skbtn":0,"sknc":0,
 "encus":210,"encn":337,"got":1920,"pdrops":0,
 "rdus":300,"qus":40,"cvus":900,"lp":1800,
 "gpus":980000,"rpart":0,"pubus":900,
 "spkb":51191040,"spks":49443840,"qfull":0,"dqf":0,
 "skecho":28,"echo":1,"ip":"192.168.1.106"}
```

### 常用的几个字段

| 字段 | 含义 | 怎么看 |
|---|---|---|
| `kind` | 这条是谁触发的：`pong` / `mic` / `gate` / `mode` / `echo` / `period` | |
| `up` | 开机多少秒 | **变小 = 板子中途重启过**。这是排查「音频断一截」的第一手证据 |
| `rssi` | Wi-Fi 信号（dBm） | −50 很好，−75 以下开始掉包 |
| `heap` | 空闲内部 RAM（字节） | 稳定就好；一直往下漏说明有泄漏 |
| `apkts` | 上行（板子发）包数 | |
| `dpkts` | 下行（板子收）包数 | 和电脑发出去的包数对比，就知道有没有丢 |
| `dbytes` | 下行收到的字节 | ÷ 32000 = 收到多少秒音频 |
| `ddrop` | 下行**因为缓冲满**丢的帧数 | >0 = 电脑送太快，缓冲被灌满 |
| `sbuf` | 喇叭缓冲里还有多少**字节** | ÷ 32 = 毫秒。播放中一直 0 = 没有余量，任何抖动都会变成断音 |
| `mic` / `gate` / `rms` / `floor` | 麦克风是否开 / 门限是否通过 / 当前音量 / 估出的房间底噪 | `rms` 高于 `floor`×3 才开闸 |
| `mode` | 0 = 室内（门限），1 = 随身（按键） | |
| `spkb` / `spks` | 写进 I2S 的**总字节** / 其中静音字节 | 每 64 字节 = 1 ms。`spkb` 含静音，**不能**当「收到的音频量」用 |
| `qfull` / `dqf` | 内部队列满的次数 | 非 0 说明处理跟不上 |
| `skecho` | 因为回声保护被跳过的块数 | 对端说话时它当然涨，正常 |

---

## 6. 心跳与探活

`STATE_PERIOD_MS` 默认 **30 秒**。原来是 5 秒，一条 JSON 约 200 字节，
一天 17280 条 ≈ 4 MB，一个月白吃掉 100 MB 配额。

**探活不要等心跳**：连上后发一条 `PING`，板子立刻回一条 `"kind":"pong"`。
客户端实现见 `host/python/audio_node_mqtt.py` 的 `Link.wait_state()`。

---

## 7. 回声保护

麦克风和喇叭装在同一个壳子里，板子放出来的声音必被自己的麦克风收进去。
所以板子在**播放期间和结束后 120 ms 内**（`SPK_TAIL_US`）不上传麦克风音频。

判据放在板子上而不是靠电脑开关麦，理由有两条：

1. 电脑一时卡住没来得及发 `MIC 0`，这段声音也不会漏进公网；
2. 将来做「边说边听」的打断功能时，这一层必须兜住「听见自己 → 又接一句」的自激。

调试打断时可以用 `ECHO 0` 关掉。

---

## 8. 本地人声门限（省流量）

麦克风开着不等于一直上传。板子自己算每一包的 RMS：

- 只在**没人说话时**学房间底噪（`floor`）。
- 门槛 = `floor × GATE_FACTOR`（3），再夹在 `[GATE_RMS_MIN, GATE_RMS_MAX]` = `[50, 250]`。
- 开口前带上 `PRE_ROLL_CHUNKS`（5 包 = 300 ms），防止吞掉第一个字。
- 说完后再传 `GATE_HANG_MS`（1.2 秒），防止吞掉尾音。

屋内实测（安静房间、无人说话）：底噪 rms 4~26，说话时 160~300。

街上/地铁里底噪比人声还大，「比底噪高 3 倍」直接失效 —— 那就用 `MODE 1` 按键说话。

---

## 9. 缓冲与节流（板子侧）

- 喇叭缓冲 `SPK_BUFFER_BYTES` = **256 KB ≈ 8 秒**单声道，放在 PSRAM。
- 播放任务**永远在往 I2S 写**，没数据就写零。不能停 —— I2S 断流会无限重放最后 DMA 里那一小段，
  听感就是「喇叭一直卡着叫同一句」。
- 电脑端必须**按实时速度**送料。一次性把 8 秒灌进去会瞬间撑满（`ddrop` 涨）然后整段丢掉。

### 下行包长为什么是 120 ms

托管 broker 按**每秒消息条数**限流。60 ms 一包 = 16.7 包/秒，实测在 EMQX Serverless
上长句的后半段会被节流到听感明显（前面的包过了，后面的挤在路上）。
120 ms 一包 = 8.3 包/秒，正好落回安全区，代价是单向多 60 ms。

对应固件里 `DOWN_MSG_MAX` / `s_asbuf` 的大小必须 ≥ `1 + 3840`。

---

## 10. 端口、TLS、防火墙

- 明文 **1883**，TLS **8883**。很多托管服务只给 8883，那就必须 `mqtts://`。
- TLS 用 IDF 内置根证书束（`esp_crt_bundle`）验证，**只要是对外签发证书的公共 CA**
  （Let's Encrypt / DigiCert / ZeroSSL）就开箱可用，不需要往工程里塞 `.pem`。自签不行。
- 板上 `MQTT_BUFFER_BYTES` 必须 > 一个音频包长度（现在是 8192，够）。
- 电脑侧**不需要开任何入站端口** —— 这是 MQTT 版相对 UDP 版最大的好处之一。

---

## 11. 自建 broker（mosquitto，5 分钟）

公共 broker 只适合试跑：同一台机器上，别人猜出你的 base 就能听你的麦克风。

```yaml
# /opt/mqtt/mosquitto.conf
listener 1883
allow_anonymous false
password_file /opt/mqtt/passwd
acl_file /opt/mqtt/acl
persistence false
```

```bash
mosquitto_passwd -c /opt/mqtt/passwd node

# /opt/mqtt/acl —— 只给自己那条前缀，别的什么都碰不到
user node
topic readwrite node-1a2b3c4d/#

docker run -d --name mqtt --restart unless-stopped \
  -p 1883:1883 -v /opt/mqtt:/opt/mqtt \
  eclipse-mosquitto:2 mosquitto -c /opt/mqtt/mosquitto.conf
```

自测（在服务器上或本机）：

```bash
mosquitto_sub -h <host> -u node -P <pass> -t 'node-1a2b3c4d/#'
mosquitto_pub -h <host> -u node -P <pass> -t 'node-1a2b3c4d/cmd' -m 'PING'
```

要公网就用 Let's Encrypt 上 TLS（`certfile` / `keyfile` / `cafile` + `listener 8883`），
然后把固件的 `BROKER_URI` 改成 `mqtts://...:8883`、`MQTT_USE_TLS` 改 `1`。

---

## 12. 实测记录（2026-10，从国内连）

| broker | 端口 | 投递率 | 备注 |
|---|---|---|---|
| `broker.hivemq.com` | 1883 明文 | 100% | 适合试跑，别长期用 |
| `broker.emqx.io` | 1883 明文 | 65% | 限流比 hivemq 严，别用 |
| EMQX Serverless（杭州） | 8883 TLS | 60ms/包时卡；120ms/包 100% | TCP 25.8 ms / TLS 53 ms，是这条链路上**最快**的 |

最后一行值得记住：**「云 platform 慢」是最容易得出、也最容易错的结论**。
那次真正的根因是「每秒消息条数」限流，和延迟无关 —— 测 TCP/TLS 往返只要 25 ms，
但长句后半段照样卡。排查时先量「每秒几个包」，别只量延迟。

---

## 13. 想扩展的话

| 想加的东西 | 怎么加 |
|---|---|
| 多台电脑同时听 | 天然支持 —— MQTT 是广播型的，多个订阅者互不影响。但只有第一个收到 `MIC 1` 的会开麦（大家共用一个状态） |
| 打断（边说边听） | 需要 `ECHO 0`，再加上回声消除，否则自己的声音会把麦克风灌满 |
| 降流量 | 下行也改用 Opus（0xAB），代价是板子要解码、播放路径多一份计算 |
| 加密（不用 TLS） | 换 WireGuard / Tailscale，broker 只监听内网 |
| 时钟同步 / 抖动缓冲 | 板子侧用 `esp_timer` 打时间戳，客户端做自适应缓冲 |

改之前先读 [architecture.md](architecture.md)：里面有几条「看起来能简化、实际会炸」的记录。
