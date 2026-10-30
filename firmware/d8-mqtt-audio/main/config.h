/* d8-mqtt-audio 的可调参数都放这里，改一个数不用翻 main.c。 */

#pragma once

/* ==================== 板型 ====================
 * 同一套固件跑两块完全不同的板，差别只在「哪几根线」。
 * 换板子只改 BOARD_PROFILE 这一个数，然后重新 build + flash。
 *
 *   BOARD_WANGXUN  现有实物：wangxun 语音板（2026-10-26 实测出来的引脚）
 *   BOARD_GOOUUU   GOOUUU 果云版 42 引脚开发板 + 面包板，
 *                  按小智 AI 那份面包板教程的「果云版」接线
 *
 * ⚠️ 两套的 BCLK/WS 是**交叉**的（4/5 ↔ 5/4），功放三根也全不一样。
 *    接错不会"声音小"，是彻底没声音，而串口日志一切正常 ——
 *    别靠听去猜，对着 docs/BODY-BUILD.md §3.2 的表量一遍。
 */
#define BOARD_WANGXUN   1
#define BOARD_GOOUUU    2
#define BOARD_PROFILE   BOARD_WANGXUN

#if BOARD_PROFILE == BOARD_WANGXUN

/* ---- 音频引脚（wangxun 语音板实测值）---- */
#define PIN_MIC_BCLK     4
#define PIN_MIC_WS       5
#define PIN_MIC_DIN      6

#define PIN_AMP_BCLK     16
#define PIN_AMP_LRC      17
#define PIN_AMP_DOUT     15

/* 板上的「录音测试」按键（一头接 IO1，一头接 GND，按下 = 低电平）。 */
#define PIN_BTN          1

#elif BOARD_PROFILE == BOARD_GOOUUU

/* ---- 音频引脚（果云版 42 引脚开发板 + 面包板，照教程 2.2 节接线）----
 *   INMP441 : SCK→GPIO5  WS→GPIO4  SD→GPIO6  VDD→3V3  GND→GND  L/R→GND
 *   MAX98357: BCLK→GPIO15 LRC→GPIO16 DIN→GPIO7 VIN→5V  GND→GND  GAIN→GND
 *             （BGA 封装的那种不要接 GAIN，接了能出声就继续用）
 *   OLED    : SDA→GPIO41 SCL→GPIO42（本固件不驱动屏幕，留着给扩展版）
 */
#define PIN_MIC_BCLK     5
#define PIN_MIC_WS       4
#define PIN_MIC_DIN      6

#define PIN_AMP_BCLK     15
#define PIN_AMP_LRC      16
#define PIN_AMP_DOUT     7

/* 按键：果云开发板上自带的 BOOT 键就是 GPIO0，一头接 GND，按下 = 低电平。
 * 用它的好处是不用额外接线；代价是**开机时按住会进下载模式**——
 * 正常使用（上电之后再按）没影响。
 * 想用面包板上另外那颗 6*6 轻触开关的话，把它接到任意空闲 GPIO（比如 1），
 * 然后把下面这行改掉。 */
#define PIN_BTN          0

#else
#error "BOARD_PROFILE 只能是 BOARD_WANGXUN 或 BOARD_GOOUUU"
#endif

#define BTN_FORCE_MS     10000   /* 室内模式：按住/按一下强制开麦 10 秒 */

/* 随身模式（MODE 1）：按一下开一个送话窗口，窗口内所有声音都上传，
 * 再按一下提前结束，到点自动结束。为什么不靠门限：
 * 街上/地铁里底噪比人声还大，"比底噪高 3 倍"这种判据直接失效。 */
#define BTN_TALK_MS      20000

/* ---- 本地人声门限（省流量/省额度）----
 * 麦克风开着的时候，板子自己算每一包的音量：安静就一个字节都不发，
 * 有人说话才往上推。这样"常开麦"不再等于"一直烧流量"。
 *
 * 门槛不是写死的：板子一边听一边估房间底噪（只在没人说话时学），
 * 门槛 = 底噪 × GATE_FACTOR，再夹在 [GATE_RMS_MIN, GATE_RMS_MAX] 之间。
 * 屋内实测（2026-10-27，安静房间、无人说话）：底噪 rms 4~26。
 * 说话时（同一位置实测）：rms 160~300。
 */
#define GATE_FACTOR      3      /* 比底噪高 3 倍算"有人说话" */
#define GATE_RMS_MIN     50     /* 门槛下限：底噪再低也别低于这个，防爆音误触 */
#define GATE_RMS_MAX     250    /* 门槛上限：屋里太吵时别把门槛抬到听不见人声 */
#define GATE_FLOOR_MIN   25     /* 底噪估计的钳位 */
#define GATE_FLOOR_MAX   120
#define GATE_HANG_MS     1200   /* 说完后继续传 1.2 秒，免得吞掉尾音 */
#define PRE_ROLL_CHUNKS  5      /* 开口前带上 5 包 = 300ms，防止吞第一个字 */

/* ---- 音频格式 ----
 * 麦克风 INMP441 只有一路声音（L/R 接地 = 数据在左声道），功放 MAX98357
 * 也只推一只喇叭 —— 走网络时没必要传立体声。
 * 所以：**线上传单声道**，板子内部 I2S 仍然是立体声（硬件要时钟）。
 * 这一个改动把上行带宽从 64 KB/s 砍到 32 KB/s，是能跑实时的关键。
 */
#define SAMPLE_RATE      16000

/* 包首字节 = 包类型。2026-10-27 起线上走 Opus：
 *   0xAB  一帧裸 Opus（120 字节左右）—— 现在两端都用这个
 *   0xAA  旧格式，包首之后是裸 PCM（1920 字节）
 * 为什么保留 0xAA：板子和 PC 分别刷/更新时一定有个窗口期两边版本不一致，
 * 认旧的至少不会一升级就彻底没声音。跑稳了以后可以删掉。
 */
#define AUDIO_MAGIC_OPUS  0xAB
#define AUDIO_MAGIC_PCM   0xAA
#define AUDIO_MAGIC_BATCH 0xAC
#define AUDIO_MAGIC       AUDIO_MAGIC_PCM     /* 发出去用哪个（2026-10-28 回裸 PCM） */

/* ---- 一个 MQTT 包里塞几帧 Opus ----
 * 2026-10-27 实测出来的硬限制：这条链路有「每秒包数」节流，跟包大小无关。
 *   PC→PC 走同一个 broker，16.7 包/秒 → 只到 55~60%；
 *   2 帧/包（8.3 包/秒）→ 94~100%；3 帧/包（5.6 包/秒）→ 100%；
 *   包大小取 20 / 40 / 60 / 121 / 200 字节，丢包率完全一样。
 * 所以把 3 帧（180 ms）合成一个包发，代价是单向多 180 ms 延迟。
 * 0xAC 包格式：[0xAC] 之后是若干个 [长度低字节 长度高字节][Opus 帧]，
 * 读到包尾为止 —— 不写死帧数，将来改帧长/帧数两端还能兼容。 */
#define AUDIO_BATCH_FRAMES 3

/* ---- 每次往 MQTT 发多长一段 ----
 * 60 ms 一包（960 帧 × 2 字节 = 1920 字节），每秒约 17 条 / 32 KB/s。
 */
#define CHUNK_FRAMES     960
#define CHUNK_BYTES      (CHUNK_FRAMES * 2)          /* 线上单声道字节数 */
#define I2S_CHUNK_BYTES  (CHUNK_FRAMES * 4)          /* 硬件立体声字节数 */

/* ---- 喇叭缓冲：收到多少先存着，断供时才补零（不断流）----
 * 64KB 单声道 ≈ 2 秒。为什么从 24KB 加大：
 *   公网 broker 的到达是"一阵一阵"的，PC 侧边合成边推时数据来不匀。
 *   缓冲只有 750ms 时，只要生成端慢半拍，播放端就被抽干 → 补零 → 听感一顿一顿。
 * 注意：PC 侧的"领先量"要小于这里，否则会溢出丢包（tools/tts_to_board.py 的 lead_ms）。
 *
 * 2026-10-27 第九修：2 秒 → 8 秒（256KB，放 PSRAM）。
 *   用户实测症状"前面 10 个字左右很流畅，后面就卡" ——
 *   10 个字 ≈ 2 秒，恰好等于 2048ms 缓冲被吃空的时间。
 *   只要 PC 端的送料率比播放率慢哪怕 5%（实测是 93.3%，因为每个 60ms 块
 *   自己还要花 4ms 做 Opus 编码 + 加锁 + TLS 发布），
 *   2 秒的缓冲就是"从开口那一刻起就在倒计时"，长句必卡在句尾。
 *   缓冲加大后，这些欠账被吸收掉，用户听不出速度差；
 *   PC 侧再用 pace=0.94 主动留 6% 余量，水位就稳定在一个高位、永远不见底。
 * 256KB 放 PSRAM（xStreamBufferCreateWithCaps），不占内部 RAM。 */
#define SPK_BUFFER_BYTES (256 * 1024)   /* ≈ 8192ms 单声道 */

/* ---- 她在说话的时候，板子不把麦克风的声音送出去（回声保护）----
 * 为什么需要：麦克风和喇叭装在一个壳子里，她念出来的声音必被麦克风收进去；
 *   而麦克风是 ingress、"屋里的声音算资料" —— 自己的声音混进去是纯噪声，
 *   而且（将来做打断时）会造成"她听见自己 → 又接一句"的自激。
 * 判据放在板子上，而不是靠 PC 侧开关麦：
 *   ① PC 一时卡住没来得及发 MIC 0 时，这段声音也不会漏进公网；
 *   ② 将来做"边说边听"的打断功能，必须靠这一层兜住自激。
 * 判据 = 喇叭缓冲里还有料，**或者**刚写完不到 SPK_TAIL_US（DMA 里还有余音）。
 * 只是为了压住自己的回声，不是静音开关：说完 SPK_TAIL_US 就自动恢复。*/
#define SPK_TAIL_US     120000      /* 120ms：写完到真放完的余量 */

/* ---- MQTT ----
 * 2026-10-27 起用自家 EMQX Serverless（杭州），TLS 8883 + 用户名密码。
 * 历史（公共 broker 实测，留个记录）：broker.emqx.io 只到 65%，hivemq 100%。
 *
 * ⚠️ Serverless 只给 TLS 端口，没有明文 1883 —— 所以必须 mqtts://
 *    证书是 DigiCert 签的公共 CA，用 IDF 内置根证书束（esp_crt_bundle）即可，
 *    不需要把任何 .pem 塞进工程。
 */
#define BROKER_URI           "mqtts://i1e78188.ala.cn-hangzhou.emqxsl.cn:8883"
#define MQTT_USE_TLS         1      /* 1 = 走 8883 + 根证书束；0 = 明文 1883 */

/* 主题前缀固定下来（原来是 MAC 派生，可被猜；现在换成随机串）。
 * 板子和 PC 必须用同一个前缀。 */
#define TOPIC_BASE           "whale-1bb62897"

#define MQTT_KEEPALIVE_S     30
#define MQTT_BUFFER_BYTES    8192   /* 必须 > CHUNK_BYTES+1 */

/* 空闲心跳周期。
 * 原来是 5 秒 —— 一条 JSON 约 200 字节，一天 17280 条 ≈ 4MB，
 * 一个月 100 多 MB，白吃掉 EMQX 免费额度（1GB/月）的 10%。
 * 现在改 30 秒，一个月约 19MB。
 * 探测板子在线不再靠等心跳：PC 连上后发一条 "PING"，板子立刻回一条 state。 */
#define STATE_PERIOD_MS      30000

/* ---- mic 任务的栈 ----
 * 它要调 Opus 编码（esp_opus_enc_process）。乐鑫那个预编译库的栈开销很大：
 *   8192 字节 —— 编第一帧就 "A stack overflow in task mic"，板子 panic 重启，
 *                 表现是"一开麦就断线、上行永远是 0 包"。
 *   32768     —— 32K×4 字节词，建任务时内部 RAM 直接不够，任务根本没起来，
 *                 表现是"计数器全是 0，连循环都没跑"。
 * 现在栈走 PSRAM（见 main.c 的 xTaskCreateWithCaps），索性给足 64 KB。 */
#define MIC_TASK_STACK       65536
