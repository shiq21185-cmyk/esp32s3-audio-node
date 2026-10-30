/*
 * e1-breadboard —— 声音走公网中转（GOOUUU 果云版 42 脚开发板 + 面包板）
 *
 * D4 那套是 UDP 点对点，只有在同一张网里才行。
 * 身体要带出去（4G / 别人的 Wi-Fi），PC 就再也找不到板子了。
 *
 * 这一版把声音搬上 MQTT：
 *
 *     麦克风 --I2S--> 板子 --publish--> <base>/audio/up   --> PC
 *     PC --publish--> <base>/audio/down --> 板子 --I2S--> 喇叭
 *
 * 两边都只做「出站连接」，谁在 NAT 后面、IP 变成多少都无所谓，
 * 只要能上网就行。公共 broker 在这里只当验证用的中转，正式要换自己的。
 *
 * 主题约定（base = whale-<MAC 后三字节>）：
 *   <base>/audio/up    板子发的麦克风 PCM，每包 1 + 1920 字节（单声道），首字节 0xAA
 *   <base>/audio/down  PC 发的喇叭 PCM，同样格式
 *   <base>/state       板子每 5 秒报一次状态（JSON 文本，含 "mic":0/1）
 *   <base>/cmd         PC 发的文本指令，比如 "MIC 1" / "MIC 0"
 *
 * 麦克风默认是**关**的：这个 broker 是公共的，主题名又由 MAC 派生，
 * 一上电就往公网推屋里声音不合适。要录就发 "MIC 1"，录完发 "MIC 0"。
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "config.h"
#include "codec.h"
#include "secrets.h"
#include "oled.h"
#include "ui.h"

static const char *TAG = "e1";

/* ---------------- Wi-Fi ---------------- */
static EventGroupHandle_t s_net_ev;
#define BIT_CONNECTED BIT0
static int s_retry;
/* 屏幕状态栏要"真的拿到 IP 了吗"，事件组只有 BIT_CONNECTED 且在别处会被读掉，
 * 所以单独留一个只写的标志，纯粹给 UI 看。 */
static volatile bool s_wifi_ok;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_retry++;
        s_wifi_ok = false;
        ESP_LOGW(TAG, "Wi-Fi 断开 (reason=%d)，第 %d 次重连…", d->reason, s_retry);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 通了，本机 IP = " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_wifi_ok = true;
        xEventGroupSetBits(s_net_ev, BIT_CONNECTED);
    }
}

static esp_err_t wifi_connect(void)
{
    s_net_ev = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_LOGI(TAG, "正在连 Wi-Fi: \"%s\" …", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_net_ev, BIT_CONNECTED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "没连上 Wi-Fi。查：名字/密码、是不是 5GHz");
        return ESP_FAIL;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* ---------------- I2S ---------------- */
static i2s_chan_handle_t s_rx;      /* 麦克风 */
static i2s_chan_handle_t s_tx;      /* 功放 */
static StreamBufferHandle_t s_spk;  /* 收到但还没播的 PCM */

static esp_err_t audio_init(void)
{
    /* 麦克风：I2S_NUM_1，只收不发 */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_MIC_BCLK, .ws = PIN_MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = PIN_MIC_DIN },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    /* 功放：auto_clear=true 让 DMA 跑空时读出来是 0，
     * 配合「永远在写」的播放任务，彻底避免"卡住重放最后一段"。 */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_AMP_BCLK, .ws = PIN_AMP_LRC,
                      .dout = PIN_AMP_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    /* 2026-10-27 第九修：缓冲从 64KB 加到 256KB（8 秒），必须放到 PSRAM，
     * 否则内部 RAM 一次就吃掉 256KB（内部只剩约 300KB），Wi-Fi/MQTT 立刻受影响。 */
    s_spk = xStreamBufferCreateWithCaps(SPK_BUFFER_BYTES, 4,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_spk == NULL) {
        ESP_LOGW(TAG, "PSRAM 建喇叭缓冲失败，退回内部 RAM（只有 2 秒）");
        s_spk = xStreamBufferCreate(64 * 1024, 4);
    }
    return ESP_OK;
}

/* 播放侧硬指标：真正写进 I2S 的字节数与静音块数。
 * 有了它才能直接算出"板子以多快速度吃缓冲"，不用再靠 PING 往返猜。*/
static volatile uint32_t s_spk_bytes;
static volatile uint32_t s_spk_silence;
/* 最近一次"真放出了声音"的时刻（微秒）。麦克风侧靠它判断"她是不是正在说话"。 */
static volatile int64_t s_spk_active_us;

/* ---------------- 她是不是正在说话 ----------------
 * 判据两条，任一成立就算"正在说"：
 *   ① 喇叭缓冲里还有料（s_spk 非空）—— 还有话没播完；
 *   ② 刚写完不到 SPK_TAIL_US —— 已经交给 DMA，但还没真放完。
 * 只用于挡住麦克风的自回声，不影响播放。ECHO 0 可以把它关掉（将来调试打断用）。
 */
static volatile bool s_echo_guard = true;
static volatile uint32_t s_skip_echo;

static bool spk_is_busy(void)
{
    if (!s_echo_guard) return false;
    /* s_spk 是 audio_init() 里才建的，而屏幕 UI 在 Wi-Fi 连上之前就起来了
     * （那会儿还没有喇叭缓冲）。不判空的话，UI 第一次取快照就会撞
     * "assert failed: xStreamBufferBytesAvailable (pxStreamBuffer)" 然后重启 ——
     * 表现是板子一直在开机、屏幕闪一下就黑。 */
    if (!s_spk) return false;
    if (xStreamBufferBytesAvailable(s_spk) > 0) return true;
    if (!s_spk_active_us) return false;
    return (esp_timer_get_time() - s_spk_active_us) < SPK_TAIL_US;
}

/* ---------------- 播放任务：永远在往 I2S 写 ---------------- */
static void spk_task(void *arg)
{
    static int16_t mono[CHUNK_FRAMES];          /* 线上收到的单声道 */
    static int16_t st[CHUNK_FRAMES * 2];        /* 喂给 I2S 的立体声 */

    while (1) {
        size_t total = xStreamBufferReceive(s_spk, mono, sizeof(mono), pdMS_TO_TICKS(30));

        if (total == 0) {
            /* 真没数据：写一整块静音占住 DMA，免得它重放上一段。 */
            memset(st, 0, sizeof(st));
            size_t written = 0;
            i2s_channel_write(s_tx, st, sizeof(st), &written, portMAX_DELAY);
            s_spk_bytes += (uint32_t)written;
            s_spk_silence += (uint32_t)written;
            continue;
        }

        /* 手上这块没灌满，就再零散捞一点，少些零头 */
        while (total < sizeof(mono)) {
            size_t n = xStreamBufferReceive(s_spk, (uint8_t *)mono + total,
                                            sizeof(mono) - total, 0);
            if (n == 0) break;
            total += n;
        }
        total = (total / 2) * 2;   /* 整帧 */
        int frames = (int)(total / 2);

        /* 单声道 → 两个声道都填同一样本（功放只推一只喇叭，做不做都响，
         * 但两边都填能让 MAX98357 的 (L+R)/2 混音拿到满幅） */
        for (int i = 0; i < frames; i++) {
            int16_t v = mono[i];
            st[i * 2] = v;
            st[i * 2 + 1] = v;
        }

        /* 只写**实际有的帧**，不补零。
         * 旧写法是补到一整块 60ms 再写 —— 缓冲只到一半时，
         * 等于每段声音后面硬塞一段静音，听感就是"一顿一顿 / 沙沙响"。 */
        size_t written = 0;
        i2s_channel_write(s_tx, st, (size_t)frames * 4, &written, portMAX_DELAY);
        s_spk_bytes += (uint32_t)written;
        /* 记下"刚真放了声音"，麦克风侧据此挡住自己的回声 */
        if (written) s_spk_active_us = esp_timer_get_time();
    }
}

/* ---------------- MQTT ---------------- */
static char s_base[40];
static char s_topic_up[64];
static char s_topic_down[64];
static char s_topic_cmd[64];
static char s_topic_state[64];

static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_connected;
static volatile bool s_mic_on;      /* 默认 false：见文件开头的说明 */
static volatile bool s_gate_open;                 /* 人声门限当前状态（给 state 看） */
static volatile int64_t s_force_until_us;         /* 按键「我现在要说」的截止时间 */
static volatile int s_last_rms;                    /* 最近一包的音量（排查门限用，随 state 报出去） */
static volatile int s_floor_noise = 30;           /* 房间底噪的慢速估计，门限跟着它走 */
static volatile bool s_gate_off;                   /* 调试/兜底：GATE 0 关掉门限，有麦就全推 */

/* ---- 两种工作模式（2026-10-27 加）----
 *   MODE_GATE(0) 室内：本地人声门限常开，有人说话才上传。屋里安静时最省流量。
 *   MODE_BTN (1) 随身：完全不看门限，只有按了按键才上传。
 *                 外面吵、门限判断不可靠，所以交给人的手指决定"这句话要不要送出去"。
 * 模式存在 NVS 里，断电重启不丢；PC 侧用 `<base>/cmd` 发 "MODE 0/1" 改。
 */
#define MODE_GATE 0
#define MODE_BTN  1

static volatile int s_mode = MODE_GATE;
static volatile int64_t s_btn_talk_until_us;       /* 随身模式：按键开启的送话窗口截止 */

static void load_mode(void)
{
    nvs_handle_t h;
    if (nvs_open("bod", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = MODE_GATE;
        if (nvs_get_u8(h, "mode", &v) == ESP_OK) s_mode = (v == 0) ? MODE_GATE : MODE_BTN;
        nvs_close(h);
    }
}

static void save_mode(int m)
{
    nvs_handle_t h;
    if (nvs_open("bod", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mode", (uint8_t)m);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint32_t s_up_pkts, s_up_fail, s_down_pkts, s_down_bytes, s_down_dropped;
/* Opus 编码统计（看"压了多少"用） */
static uint32_t s_up_enc_pkts, s_up_enc_bytes, s_up_enc_fail;
/* 排查用：mic_task 到底跑了几轮、读 I2S 有没有成功、发到 publish_pcm_chunk 几次 */
static volatile uint32_t s_mic_loops, s_mic_reads, s_mic_pub;
static volatile uint32_t s_skip_off, s_skip_gate, s_skip_btn, s_skip_nocon;
/* 计时统计：看 Opus 编码吃多少 CPU、I2S 每次读到多少字节 */
static volatile uint32_t s_enc_us_total, s_enc_calls, s_last_got;
static volatile uint32_t s_pub_us, s_pub_n;
/* 时间都花在哪：i2s 读等多久、mbuf 塞多久 */
static volatile uint32_t s_read_us, s_queue_us;
static volatile uint32_t s_conv_us, s_gate_us, s_loop_us;static volatile uint32_t s_reads_partial;
static volatile int64_t s_gap_us;
static volatile int64_t s_prev_end_us;
/* 上行 PCM 环形缓冲：mic_task 只往里塞，enc_task 只管编码发出去。
 * 塞不下说明编码端跟不上 —— 记到 drops 里，不要阻塞采集（阻塞 = 越说越滞后）。*/
static StreamBufferHandle_t s_pcm_up;
static volatile uint32_t s_pcm_drops;

/* ---------------- 下行队列（前向声明）----------------
 * publish_state() 要把队列积压数上报到 state，所以这两个必须在它之前声明。 */
static QueueHandle_t s_down_q;
static uint32_t s_down_qfull;

static void publish_state(const char *kind)
{
    /* 🔴 2026-10-28：这个缓冲区曾经是 512 字节。
     * 随着调试计数器越加越多（spkb/spks/qfull/dqf/skecho/echo…），
     * JSON 被 snprintf 静默截断在 511 字节，结尾停在 `"ip":"192` —— 
     * **JSON 不完整**，PC 侧 json.loads 直接抛异常，于是：
     *   · body_mqtt 的 self._state 永远是空
     *   · wait_state() 返回 {} ⇒ body_probe 判"板子没开机" ⇒ 一键启动整段跳过身体
     *   · sbuf_ms() 返回 None ⇒ 水位自适应失效
     * 症状极具误导性：板子明明在线、音频上下行都正常，但 PC 说"没等到心跳"。
     * 现在给到 768，并且**检查 snprintf 的返回值**，截断时大声报错而不是静默。
     * 以后再加字段，看串口有没有 STATE JSON TRUNCATED 就知道够不够。 */
    char payload[768];
    wifi_ap_record_t ap = { 0 };
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (netif) esp_netif_get_ip_info(netif, &ip);

    int n = snprintf(payload, sizeof(payload),
             "{\"kind\":\"%s\",\"up\":%lu,\"rssi\":%d,\"heap\":%lu,\"apkts\":%lu,"
             "\"dpkts\":%lu,\"dbytes\":%lu,\"ddrop\":%lu,\"sbuf\":%d,\"mic\":%d,\"gate\":%d,\"rms\":%d,\"floor\":%d,\"mode\":%d,"
             "\"enc\":%lu,\"encb\":%lu,\"encfail\":%lu,"
             "\"mloop\":%lu,\"mread\":%lu,\"mpub\":%lu,"
             "\"skoff\":%lu,\"skgate\":%lu,\"skbtn\":%lu,\"sknc\":%lu,"
             "\"encus\":%lu,\"encn\":%lu,\"got\":%lu,\"pdrops\":%lu,\"rdus\":%lu,\"qus\":%lu,\"cvus\":%lu,\"lp\":%lu,\"gpus\":%lld,\"rpart\":%lu,\"pubus\":%lu,"
             "\"spkb\":%lu,\"spks\":%lu,\"qfull\":%lu,\"dqf\":%u,\"skecho\":%lu,\"echo\":%d,"
             "\"ip\":\"" IPSTR "\"}",
             kind, (unsigned long)(esp_timer_get_time() / 1000000), rssi,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)s_up_pkts, (unsigned long)s_down_pkts,
             (unsigned long)s_down_bytes, (unsigned long)s_down_dropped,
             (int)xStreamBufferBytesAvailable(s_spk), s_mic_on ? 1 : 0,
             s_gate_open ? 1 : 0, (int)s_last_rms, (int)s_floor_noise, s_mode,
             (unsigned long)s_up_enc_pkts, (unsigned long)s_up_enc_bytes,
             (unsigned long)s_up_enc_fail,
             (unsigned long)s_mic_loops, (unsigned long)s_mic_reads,
             (unsigned long)s_mic_pub,
             (unsigned long)s_skip_off, (unsigned long)s_skip_gate,
             (unsigned long)s_skip_btn, (unsigned long)s_skip_nocon,
             (unsigned long)s_enc_us_total, (unsigned long)s_enc_calls,
             (unsigned long)s_last_got, (unsigned long)s_pcm_drops,
             (unsigned long)s_read_us, (unsigned long)s_queue_us,
             (unsigned long)s_conv_us, (unsigned long)s_loop_us,
             (long long)s_gap_us, (unsigned long)s_reads_partial,
             (unsigned long)(s_pub_n ? s_pub_us / s_pub_n : 0),
             (unsigned long)s_spk_bytes, (unsigned long)s_spk_silence,
             (unsigned long)s_down_qfull,
             (unsigned)s_down_q ? (unsigned)uxQueueMessagesWaiting(s_down_q) : 0u,
             (unsigned long)s_skip_echo, s_echo_guard ? 1 : 0,
             IP2STR(&ip.ip));

    /* snprintf 返回"原本想写多少字节"。>= sizeof 就是被截断了。
     * 截断 = 发出去的 JSON 不完整 = PC 侧解析失败 = 链路看起来"板子不在线"。
     * 所以这里必须吵一次，别让它又静默下去。 */
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        ESP_LOGE(TAG, "STATE JSON TRUNCATED: need %d bytes, buffer %u "
                      "(PC side will fail to parse! grow payload[])",
                 n, (unsigned)sizeof(payload));
    }

    esp_mqtt_client_publish(s_mqtt, s_topic_state, payload, 0, 0, 0);
}

/* 下行的音频包可能被拆成几次事件送进来，先攒在 s_asbuf 里。
 * 2160 = 3 帧 120ms 的打包（每帧 ≤240 字节 + 2 字节长度头）再留足余量。
 *
 * ⚠️ 2026-10-28：PC 侧把裸 PCM 的包从 60ms 加大到 120ms（EMQX 卡消息条数，
 * 见 tools/body_mqtt.py 的 CHUNK_FRAMES 注释），所以一包 PCM 是
 * 1 + 1920×2 = 3841 字节。这个数组和下面的 DOWN_MSG_MAX 都必须 ≥ 那个值，
 * 否则大包会被静默丢弃（s_aslen 重置），听起来就是"每隔一包断一下"。 */
static uint8_t s_asbuf[8192];
static int s_aslen;

/* ⚠️ 2026-10-27 血泪：**解码绝对不能放在 mqtt_task 里做**。
 * 3 帧一包时，在 MQTT 回调线程里连解 3 帧直接把 mqtt_task 的栈撑爆：
 *   ***ERROR*** A stack overflow in task mqtt_task has been detected.
 *   Backtrace: ... |<-CORRUPTED
 * 症状是"发一段话板子就重启"，计数归零、声音全无，而且串口不仔细看根本发现不了。
 * 所以现在：MQTT 回调只把整包拷进队列就返回（memcpy 而已），
 * 真正解码交给独立的 dec_task。 */
#define DOWN_Q_ITEMS 32
#define DOWN_MSG_MAX 4096

typedef struct {
    uint16_t len;
    uint8_t  data[DOWN_MSG_MAX];
} down_msg_t;

static void handle_audio_down(const esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) s_aslen = 0;

    int n = ev->data_len;
    if (n > 0 && s_aslen + n <= (int)sizeof(s_asbuf)) {
        memcpy(s_asbuf + s_aslen, ev->data, n);
        s_aslen += n;
    } else {
        s_aslen = 0;
    }

    if (ev->current_data_offset + ev->data_len < ev->total_data_len) {
        return;   /* 还没收全 */
    }

    if (s_aslen < 2) {
        s_aslen = 0;
        return;
    }

    /* MQTT 回调里只做搬运：整包拷进队列立刻返回。解码在 dec_task 里做，
     * 这里绝不能碰 codec_decode（栈不够，见上面的血泪注释）。
     *
     * ⚠️ 2026-10-28：DOWN_MSG_MAX 从 2048 加到 4096（裸 PCM 改 120ms/包），
     * 这个结构体就 4KB 了 —— **不能放栈上**，否则又要把 mqtt_task 撑爆。
     * 所以改成 static：回调是单线程串行的（esp-mqtt 事件走同一个 task），
     * 用 static 不会重入，且不占栈。 */
    static down_msg_t m;
    if (s_aslen > DOWN_MSG_MAX) s_aslen = DOWN_MSG_MAX;
    m.len = (uint16_t)s_aslen;
    memcpy(m.data, s_asbuf, s_aslen);
    s_aslen = 0;
    /* 队列给 1 个 tick 的耐心：以前是 0（立刻放弃），公网那种"一次冲进来 20 包"
     * 的突发下会成批丢掉 —— 实测灌 20 包能丢 15 帧，而 ddrop 一个都不涨，
     * 所以从计数上完全看不出来，只能从"音频变短"察觉。
     * 现在给 1 tick：解码任务很快就能腾出一条空位，基本不会再丢；
     * 真丢了会记到 qfull 里并上报到 state（见 publish_state 的 "qfull"）。 */
    if (xQueueSend(s_down_q, &m, 1) != pdTRUE) {
        s_down_qfull++;
    }
}

/* ---------- 解码任务：把队列里的音频包解成 PCM 塞进喇叭缓冲 ---------- */
static void dec_task(void *arg)
{
    static down_msg_t m;
    static int16_t pcm[CODEC_FRAME_SAMPLES];

    while (1) {
        if (xQueueReceive(s_down_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.len < 2) continue;

        uint8_t kind = m.data[0];

        if (kind == AUDIO_MAGIC_BATCH) {
            /* 打包格式：[0xAC] 之后是若干组 [len_lo len_hi][Opus 帧]。
             * 一包里的帧必须按顺序喂给解码器（Opus 有状态）。 */
            int off = 1;
            int frames = 0;
            while (off + 2 <= (int)m.len) {
                int flen = (int)m.data[off] | ((int)m.data[off + 1] << 8);
                off += 2;
                if (flen <= 0 || off + flen > (int)m.len) break;   /* 残缺包 */
                int got = codec_decode(m.data + off, flen, pcm, CODEC_FRAME_SAMPLES);
                off += flen;
                frames++;
                if (got > 0) {
                    size_t bytes = (size_t)got * 2;
                    if (xStreamBufferSend(s_spk, pcm, bytes, 0) != bytes) {
                        s_down_dropped++;
                    }
                    s_down_bytes += bytes;
                } else {
                    s_down_dropped++;
                }
            }
            s_down_pkts += frames;
        } else if (kind == AUDIO_MAGIC_OPUS) {
            /* 一包一帧 Opus（兼容老格式） */
            int got = codec_decode(m.data + 1, (int)m.len - 1, pcm, CODEC_FRAME_SAMPLES);
            if (got > 0) {
                size_t bytes = (size_t)got * 2;
                if (xStreamBufferSend(s_spk, pcm, bytes, 0) != bytes) {
                    s_down_dropped++;
                }
                s_down_bytes += bytes;
            } else {
                s_down_dropped++;
            }
            s_down_pkts++;
        } else if (kind == AUDIO_MAGIC_PCM) {
            size_t len = (size_t)(m.len - 1) & ~1u;   /* 单声道整帧 */
            if (len > 0) {
                if (xStreamBufferSend(s_spk, m.data + 1, len, 0) != len) {
                    s_down_dropped++;
                }
                s_down_bytes += len;
            }
            s_down_pkts++;
        }
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        esp_mqtt_client_subscribe(s_mqtt, s_topic_down, 0);
        esp_mqtt_client_subscribe(s_mqtt, s_topic_cmd, 0);
        ESP_LOGI(TAG, "==================================");
        ESP_LOGI(TAG, "  连上 broker！主题前缀 = %s", s_base);
        ESP_LOGI(TAG, "  上行 %s", s_topic_up);
        ESP_LOGI(TAG, "  下行 %s", s_topic_down);
        ESP_LOGI(TAG, "==================================");
        publish_state("hello");
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "和 broker 断开，会自动重连");
        break;

    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(s_topic_down) &&
            memcmp(ev->topic, s_topic_down, ev->topic_len) == 0) {
            handle_audio_down(ev);
        } else if (ev->topic_len == (int)strlen(s_topic_cmd) &&
                   memcmp(ev->topic, s_topic_cmd, ev->topic_len) == 0) {
            char cmd[128] = { 0 };
            int n = ev->data_len < (int)sizeof(cmd) - 1 ? ev->data_len : (int)sizeof(cmd) - 1;
            memcpy(cmd, ev->data, n);
            /* 「MIC 1」开、「MIC 0」关。录完必须关：不关就一直往公网推屋里声音。 */
            if (strncmp(cmd, "MIC ", 4) == 0) {
                bool want = (cmd[4] != '0');
                if (want != s_mic_on) {
                    s_mic_on = want;
                    ESP_LOGI(TAG, "麦克风%s", want ? "开" : "关");
                    publish_state("mic");
                }
            } else if (strncmp(cmd, "GATE ", 5) == 0) {
                s_gate_off = (cmd[5] == '0');
                ESP_LOGI(TAG, "人声门限 %s", s_gate_off ? "关" : "开");
                publish_state("gate");
            } else if (strncmp(cmd, "MODE ", 5) == 0) {
                int m = (cmd[5] == '1') ? MODE_BTN : MODE_GATE;
                if (m != s_mode) {
                    s_mode = m;
                    save_mode(m);
                    s_btn_talk_until_us = 0;
                    ESP_LOGI(TAG, "模式切成 %s", m == MODE_BTN ? "随身（按键说话）"
                                                              : "室内（人声门限）");
                    publish_state("mode");
                }
            } else if (strncmp(cmd, "PING", 4) == 0) {
                /* PC 侧连上就问一句，省得等 30 秒心跳才能判断板子在不在 */
                publish_state("pong");
            } else if (strncmp(cmd, "ECHO ", 5) == 0) {
                /* ECHO 0 关掉"她说话时麦克风闭嘴"的保护（调试打断用，平时别关） */
                bool want = (cmd[5] != '0');
                if (want != s_echo_guard) {
                    s_echo_guard = want;
                    ESP_LOGI(TAG, "回声保护%s", want ? "开" : "关");
                    publish_state("echo");
                }
            } else {
                ESP_LOGI(TAG, "收到指令: %s", cmd);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 出错（网络抖动会触发，通常自动恢复）");
        break;

    default:
        break;
    }
}

/* ---------------- 采集任务：读了就往 broker 发 ---------------- */
/* 一包 PCM 的 RMS。用来判断"这会儿有没有人说话"。 */
static int chunk_rms(const uint8_t *mono, size_t len)
{
    const int16_t *p = (const int16_t *)mono;
    size_t n = len / 2;
    if (n == 0) return 0;
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = p[i];
        acc += (int64_t)s * s;
    }
    return (int)sqrt((double)(acc / (int64_t)n));
}

/* 把一块单声道 PCM（CHUNK_BYTES = 960 样本 = 60ms，正好一帧 Opus）
 * 塞进上行缓冲就返回，绝不在这里编码 —— 编码要 24ms，会拖慢 I2S 采集。 */
static void queue_pcm_chunk(const uint8_t *mono, int len)
{
    if (len <= 0) return;
    size_t put = xStreamBufferSend(s_pcm_up, mono, (size_t)len, 0);
    if (put < (size_t)len) s_pcm_drops++;
}

/* ---------- 编码任务：从上行的 PCM 缓冲取整帧 → Opus → 攒够 N 帧打包发出去 ---------- */
static void enc_task(void *arg)
{
    static uint8_t pf[CODEC_FRAME_BYTES];   /* 960 样本 × 2 字节 */
    static uint8_t pkt[CODEC_PKT_MAX];      /* 单帧 Opus */
    /* 打包缓冲：首字节 0xAC，之后每帧 [len_lo len_hi][payload...] */
    static uint8_t batch[1 + AUDIO_BATCH_FRAMES * (CODEC_PKT_MAX + 2)];
    int batch_len = 1;                      /* 已攒的字节数，1 = 只有包头 */
    int batch_frames = 0;

    size_t fill = 0;
    while (1) {
        /* ⚠️ xStreamBufferReceive 一旦有数据就返回，拿到的是"这次能给多少"，
         *    不保证凑满一帧。原版写了 `if (got != FRAME) continue;`
         *    → 凡是没凑满的那次全丢，实测整整丢掉一半音频（52% 实时）。
         *    正确做法是攒够 1920 字节再编。 */
        size_t got = xStreamBufferReceive(s_pcm_up, pf + fill,
                                          CODEC_FRAME_BYTES - fill,
                                          pdMS_TO_TICKS(200));
        if (got == 0) {
            /* 卡了 200ms 没新数据 —— 半截的打包先发掉，不然这几帧要等到下一句 */
            if (batch_frames > 0) goto flush_batch;
            continue;
        }
        fill += got;
        if (fill < CODEC_FRAME_BYTES) continue;
        fill = 0;

        int64_t t0 = esp_timer_get_time();
        int n = codec_encode((const int16_t *)pf, CODEC_FRAME_SAMPLES,
                             pkt, CODEC_PKT_MAX);
        s_enc_us_total += (uint32_t)(esp_timer_get_time() - t0);
        s_enc_calls++;

        if (n <= 0) {
            /* 编码失败就退回发裸 PCM（0xAA），宁可费流量也不让话丢在半路。
             * 打包里已经有东西的话，先把它发掉再走兜底，别把顺序搞乱。 */
            if (batch_frames > 0) {
                batch[0] = AUDIO_MAGIC_BATCH;
                if (esp_mqtt_client_publish(s_mqtt, s_topic_up,
                                            (const char *)batch, batch_len, 0, 0) >= 0) {
                    s_up_pkts++;
                } else {
                    s_up_fail++;
                }
                batch_len = 1;
                batch_frames = 0;
            }
            s_up_enc_fail++;
            pkt[0] = AUDIO_MAGIC_PCM;
            memcpy(pkt + 1, pf, CODEC_FRAME_BYTES);
            if (esp_mqtt_client_publish(s_mqtt, s_topic_up, (const char *)pkt,
                                        CODEC_FRAME_BYTES + 1, 0, 0) >= 0) {
                s_up_pkts++;
            } else {
                s_up_fail++;
            }
            continue;
        }

        s_up_enc_pkts++;
        s_up_enc_bytes += n;

        /* 塞进打包缓冲：[len_lo len_hi][payload] */
        if (batch_len + n + 2 > (int)sizeof(batch)) goto flush_batch;
        batch[batch_len++] = (uint8_t)(n & 0xFF);
        batch[batch_len++] = (uint8_t)((n >> 8) & 0xFF);
        memcpy(batch + batch_len, pkt, n);
        batch_len += n;
        batch_frames++;

        if (batch_frames < AUDIO_BATCH_FRAMES) continue;

    flush_batch:
        if (batch_frames > 0) {
            batch[0] = AUDIO_MAGIC_BATCH;
            int64_t tp0 = esp_timer_get_time();
            int pubr = esp_mqtt_client_publish(s_mqtt, s_topic_up,
                                               (const char *)batch, batch_len, 0, 0);
            s_pub_us += (uint32_t)(esp_timer_get_time() - tp0);
            s_pub_n++;
            if (pubr >= 0) {
                s_up_pkts++;
            } else {
                s_up_fail++;
            }
            batch_len = 1;
            batch_frames = 0;
        }
    }
}

static void mic_task(void *arg)
{
    static uint8_t st[I2S_CHUNK_BYTES];   /* I2S 出来是立体声 3840 字节 */
    static uint8_t mono[CHUNK_BYTES];     /* 抽左声道后 1920 字节 */
    static uint8_t pre[PRE_ROLL_CHUNKS][CHUNK_BYTES];  /* 开口前的环形缓冲 */
    int pre_head = 0, pre_cnt = 0;
    int64_t last_voice_us = 0;

    while (1) {
        size_t got = 0;
        int64_t t_read0 = esp_timer_get_time();
        if (s_prev_end_us) s_gap_us += (t_read0 - s_prev_end_us);
        s_mic_loops++;
        esp_err_t rerr = i2s_channel_read(s_rx, st, sizeof(st), &got, 1000);
        s_read_us += (uint32_t)(esp_timer_get_time() - t_read0);
        if (rerr != ESP_OK || got < 4) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        s_mic_reads++;
        s_last_got = (uint32_t)got;
        if (got != sizeof(st)) s_reads_partial++;

        /* 立体声 → 单声道（只取左声道，右声道是空的） */
        int64_t t_c0 = esp_timer_get_time();
        size_t frames = got / 4;
        for (size_t i = 0; i < frames; i++) {
            mono[i * 2]     = st[i * 4];
            mono[i * 2 + 1] = st[i * 4 + 1];
        }
        size_t mono_len = frames * 2;

        if (!s_connected) {
            s_skip_nocon++;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 麦克风关着就只是把 I2S 读掉（让 DMA 继续流动），一个字节都不往外发。
         * 数据已经读出来了，直接丢掉，不加延时 —— 加了会让读取节奏乱掉。 */
        if (!s_mic_on) {
            s_skip_off++;
            pre_head = 0;
            pre_cnt = 0;
            last_voice_us = 0;
            s_gate_open = false;
            continue;
        }

        /* ---- 她正在说话 ⇒ 这段不送出去（echo guard）----
         * 麦克风和喇叭在同一个壳子里，她说的话必被自己听见。等她闭嘴
         * SPK_TAIL_US 之后再开，否则会把她的尾音切成一句"半截话"。
         * 注意这里必须**继续把 I2S 读掉**（上面已经读完了），只是不发送。 */
        if (spk_is_busy()) {
            s_skip_echo++;
            pre_head = 0;
            pre_cnt = 0;
            last_voice_us = 0;
            s_gate_open = false;
            continue;
        }

        /* ---- 上传判定：两种模式分别是这样 ----
         *   室内(0)：靠本地人声门限，安静就一个字节都不发。
         *   随身(1)：完全看按键窗口，窗口里所有声音都发、窗口外一个字节都不发。
         *            外面吵的时候"音量判断有没有人说话"根本不成立，交给手指决定。 */
        int64_t now = esp_timer_get_time();
        int rms = chunk_rms(mono, mono_len);
        s_last_rms = rms;
        s_conv_us += (uint32_t)(esp_timer_get_time() - t_c0);

        if (s_mode == MODE_BTN) {
            if (now >= s_btn_talk_until_us) {
                s_skip_btn++;
                s_gate_open = false;
                pre_head = 0;
                pre_cnt = 0;
                continue;
            }
        } else {
            /* 底噪自适应：只在门限关着的时候学（开着说明是自己人在说话，学了会把门槛顶高）。
             * 屋里本来吵（空调、风扇、街上）时门槛自动抬高，安静时自动降下来。 */
            if (!s_gate_open) {
                int f = (s_floor_noise * 15 + rms) / 16;
                if (f < GATE_FLOOR_MIN) f = GATE_FLOOR_MIN;
                if (f > GATE_FLOOR_MAX) f = GATE_FLOOR_MAX;
                s_floor_noise = f;
            }
            int thr = s_floor_noise * GATE_FACTOR;
            if (thr < GATE_RMS_MIN) thr = GATE_RMS_MIN;
            if (thr > GATE_RMS_MAX) thr = GATE_RMS_MAX;
            if (rms >= thr) last_voice_us = now;

            bool forced = (now < s_force_until_us) || s_gate_off;
            bool open = forced ||
                        (last_voice_us != 0 &&
                         (now - last_voice_us) < (int64_t)GATE_HANG_MS * 1000);

            if (!open) {
                s_skip_gate++;
                if (mono_len == CHUNK_BYTES) {
                    memcpy(pre[pre_head], mono, CHUNK_BYTES);
                    pre_head = (pre_head + 1) % PRE_ROLL_CHUNKS;
                    if (pre_cnt < PRE_ROLL_CHUNKS) pre_cnt++;
                }
                s_gate_open = false;
                continue;
            }
        }

        /* 刚开口：先把开口前存的那几包补发，免得第一个字被切掉（只有室内模式会囤） */
        if (s_mode == MODE_GATE && !s_gate_open && pre_cnt > 0) {
            for (int k = 0; k < pre_cnt; k++) {
                int idx = (pre_head - pre_cnt + k + PRE_ROLL_CHUNKS * 2) % PRE_ROLL_CHUNKS;
                queue_pcm_chunk(pre[idx], CHUNK_BYTES);
            }
            pre_cnt = 0;
            pre_head = 0;
        }
        s_gate_open = true;

        int64_t t_q0 = esp_timer_get_time();
        queue_pcm_chunk(mono, (int)mono_len);
        s_mic_pub++;
        uint32_t qus = (uint32_t)(esp_timer_get_time() - t_q0);
        s_queue_us += qus;
        s_prev_end_us = esp_timer_get_time();
        s_loop_us += (uint32_t)(s_prev_end_us - t_read0);

    }
}

/* 「录音测试」按键：按一下 = 「我现在要说」，强制开麦 10 秒。
 * 适合屋里本来就吵、门限不灵的时候，或者就是想明确地说一句。 */
static void btn_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int last = 1;
    while (1) {
        int lv = gpio_get_level(PIN_BTN);
        if (last == 1 && lv == 0) {
            int64_t now = esp_timer_get_time();
            if (s_mode == MODE_BTN) {
                if (now < s_btn_talk_until_us) {
                    s_btn_talk_until_us = 0;              /* 再按一下 = 我说完了 */
                    ESP_LOGI(TAG, "按键：结束送话");
                } else {
                    s_btn_talk_until_us = now + (int64_t)BTN_TALK_MS * 1000;
                    s_mic_on = true;                      /* 按键本身就是"我要说"，不等 PC 来开麦 */
                    ESP_LOGI(TAG, "按键：开始送话 %d 秒（再按一下可提前结束）",
                             BTN_TALK_MS / 1000);
                }
            } else {
                s_force_until_us = now + (int64_t)BTN_FORCE_MS * 1000;
                ESP_LOGI(TAG, "按键：强制开麦 %d 秒", BTN_FORCE_MS / 1000);
            }
            if (s_connected) publish_state("btn");
        }
        last = lv;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}


/* 每 5 秒打一行统计。⚠️ 单独一个任务：如果放在 mic_task 里，
 *    mic_task 一卡住日志就跟着停，正好把要观察的现象掩盖掉。 */
static void reporter_task(void *arg)
{
    (void)arg;
    uint32_t p_apkts = 0, p_enc = 0, p_mic_loops = 0;
    uint32_t p_read_us = 0, p_enc_us = 0, p_enc_n = 0;
    uint32_t p_gap = 0, p_loop = 0, p_gpus_n = 0;
    int64_t t0 = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t t1 = esp_timer_get_time();
        double dt = (double)(t1 - t0) / 1e6;
        t0 = t1;
        uint32_t apkts = s_up_enc_pkts, enc = s_up_enc_bytes;
        uint32_t ml = s_mic_loops, rd = s_read_us;
        uint32_t eu = s_enc_us_total, en = s_enc_calls;
        int64_t gp = s_gap_us; uint32_t lp = s_loop_us;

        ESP_LOGI(TAG, "[%.1fs] 包 %lu (%.1f/s) | 音频 %.2fs (%.0f%%) | mic 轮 %lu (%.1f/s) "
                      "| read %.1fms/轮 loop %.1fms/轮 gap %.1fms/轮 | opus %.1fms/帧 "
                      "| pub %.1fms",
                 dt,
                 (unsigned long)(apkts - p_apkts), (apkts - p_apkts) / dt,
                 (apkts - p_apkts) * 0.06, 100.0 * (apkts - p_apkts) * 0.06 / dt,
                 (unsigned long)(ml - p_mic_loops), (ml - p_mic_loops) / dt,
                 (ml - p_mic_loops) ? (double)(rd - p_read_us) / (ml - p_mic_loops) / 1000.0 : 0.0,
                 (ml - p_mic_loops) ? (double)(lp - p_loop) / (ml - p_mic_loops) / 1000.0 : 0.0,
                 (ml - p_mic_loops) ? (double)(gp - p_gap) / (ml - p_mic_loops) / 1000.0 : 0.0,
                 (en - p_enc_n) ? (double)(eu - p_enc_us) / (en - p_enc_n) / 1000.0 : 0.0,
                 s_pub_n ? (double)s_pub_us / s_pub_n / 1000.0 : 0.0);
        (void)enc; (void)p_enc; (void)p_enc_us; (void)p_enc_n; (void)p_gpus_n;
        p_apkts = apkts; p_enc = enc; p_mic_loops = ml; p_read_us = rd;
        p_enc_us = eu; p_enc_n = en; p_gap = (uint32_t)gp; p_loop = lp;
    }
}

/* 屏幕每 100ms 来取一次快照。这里只读、不加锁：
 * 这些量要么是 bool / int 的单个字（32 位读写天然原子），
 * 要么读到的稍旧一点也无所谓（差 100ms 的电平条看不出来）。 */
static ui_status_t ui_snapshot(void)
{
    ui_status_t st = { 0 };
    st.wifi_ok    = s_wifi_ok;
    st.mqtt_ok    = s_connected;
    st.mic_on     = s_mic_on;
    st.gate_open  = s_gate_open;
    st.btn_window = (s_mode == MODE_BTN) && (s_btn_talk_until_us > 0) &&
                    (esp_timer_get_time() < s_btn_talk_until_us);
    st.speaking   = spk_is_busy();
    st.rms        = s_last_rms;
    st.floor      = s_floor_noise;
    st.mode       = s_mode;
    st.up_pkts    = s_up_pkts;
    st.down_pkts  = s_down_pkts;
    if (s_wifi_ok) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = { 0 };
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
            snprintf(st.ip, sizeof(st.ip), IPSTR, IP2STR(&ip.ip));
        }
    }
    return st;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 上一次设的模式（室内/随身）存在 NVS 里，重启不用 PC 再交代一遍 */
    load_mode();
    ESP_LOGI(TAG, "工作模式：%s", s_mode == MODE_BTN ? "随身（按键说话）" : "室内（人声门限）");

    /* 主题前缀固定在 config.h（原来是 MAC 后三位派生，容易被猜） */
    snprintf(s_base, sizeof(s_base), "%s", TOPIC_BASE);
    snprintf(s_topic_up, sizeof(s_topic_up), "%s/audio/up", s_base);
    snprintf(s_topic_down, sizeof(s_topic_down), "%s/audio/down", s_base);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/cmd", s_base);
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/state", s_base);
    ESP_LOGI(TAG, "主题前缀 = %s", s_base);

    /* 屏幕先起来：下面 wifi_connect() 要阻塞十几秒，这段时间有张脸比黑屏好。
     * 没接屏（探不到 0x3C）时 oled_init() 返回 false，UI 就不启动，
     * 后面的音频/MQTT 完全不受影响 —— 屏幕是可选件。 */
    if (oled_init()) {
        ui_start(ui_snapshot);
    }

    if (wifi_connect() != ESP_OK) return;

    if (audio_init() != ESP_OK) return;
    ESP_LOGI(TAG, "麦克风 %d/%d/%d，功放 %d/%d/%d，%d Hz",
             PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_DIN,
             PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DOUT, SAMPLE_RATE);

    /* Opus 编解码器（线上就是靠它把 32 KB/s 压到 2 KB/s） */
    if (!codec_init()) {
        ESP_LOGE(TAG, "Opus 起不来，只能跑裸 PCM（费流量 16 倍）");
    }

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.uri = BROKER_URI;
    cfg.session.keepalive = MQTT_KEEPALIVE_S;
    cfg.network.reconnect_timeout_ms = 3000;
    /* client_id 加 board- 前缀：PC 侧也用同一个主题前缀，但 client_id 不能一样，
     * 否则两边会互相把对方顶下线（MQTT 同 id 后到者踢前者）。 */
    static char s_client_id[48];
    snprintf(s_client_id, sizeof(s_client_id), "board-%s", s_base);
    cfg.credentials.client_id = s_client_id;
    /* 公共 broker 是匿名的：账号为空时**不能**把空串当用户名发出去，
     * 有些 broker 会当成"提供了错误的用户名"直接拒绝连接。 */
    if (MQTT_USER[0] != '\0') {
        cfg.credentials.username = MQTT_USER;
        cfg.credentials.authentication.password = MQTT_PASS;
    }
#if MQTT_USE_TLS
    /* Serverless 的证书是 DigiCert 公共 CA 签的，用 IDF 内置根证书束即可 */
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    cfg.buffer.size = MQTT_BUFFER_BYTES;
    cfg.buffer.out_size = MQTT_BUFFER_BYTES;

    ESP_LOGI(TAG, "正在连中转 %s …", BROKER_URI);
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));

    /* 上行 PCM 缓冲：8 帧 = 480ms 余量。编码端偶尔慢半拍也不至于丢，
     * 但也不能太大，否则一卡就积压、越积越晚（延迟比丢包更难听）。 */
    s_pcm_up = xStreamBufferCreate(CODEC_FRAME_BYTES * 8, 4);
    /* 解码队列 + dec 任务：MQTT 回调只搬运，解码在这里做。
     * down_msg_t 有 2KB。队列从 6 条加到 32 条（≈5.8 秒音频），
     * 因为实测「一口气冲进 20 包」时 6 条队列会成批丢——
     * 而那种成批到达正是公网抖动的常态。64KB 也从 PSRAM 拿，别占内部 RAM。 */
    s_down_q = xQueueCreateWithCaps(DOWN_Q_ITEMS, sizeof(down_msg_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_down_q) {
        ESP_LOGE(TAG, "下行音频队列建不出来");
    }
    if (xTaskCreateWithCaps(dec_task, "dec", 40960, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "dec 任务建不出来（PSRAM 栈）");
    }
    /* 编码器和 mic 一样吃栈（乐鑫预编译库占用极大），栈同样放 PSRAM。 */
    if (xTaskCreateWithCaps(enc_task, "enc", 40960, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "enc 任务建不出来（PSRAM 栈）");
    }
    xTaskCreate(spk_task, "spk", 4096, NULL, 6, NULL);
    /* mic 任务现在**不做编码**了（只读 I2S + 抽单声道 + 判定 + 塞缓冲），
     * 内部 RAM 8KB 就够。以前把栈放 PSRAM 是因为要在栈上跑 Opus —— 搬走之后
     * PSRAM 反而成了拖累：每次栈访问都过 cache，读 60ms 音频要花 100ms。
     * 教训：能放内部 RAM 的任务别放 PSRAM。 */
    BaseType_t ok = xTaskCreate(mic_task, "mic", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "mic 任务建不出来");
        xTaskCreate(mic_task, "mic", 8192, NULL, 5, NULL);
    }
    xTaskCreate(btn_task, "btn", 3072, NULL, 3, NULL);
    xTaskCreate(reporter_task, "rep", 4096, NULL, 2, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATE_PERIOD_MS));
        if (s_connected) publish_state("tick");
    }
}
