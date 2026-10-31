/* e2-camera —— OV7670 画面走 MQTT（跨网，板子连 WiFi、电脑在别的网）
 *
 * 为什么不是 HTTP：电脑在实验室有线网 192.168.77.x，板子在室内 WiFi 192.168.1.x，
 * 两个网段互不相通（实测 ping 100% 丢包）。所以沿用音频那套思路 ——
 * 两边都主动往外连 broker，谁在什么网后面都无所谓。
 *
 * 用法：
 *   烧进去后，电脑端跑：
 *       <她的 venv python> tools/cam_view.py --save .tmp/cam/live.jpg
 *   想一直看就加 --loop（每次收到就覆盖 .tmp/cam/live.jpg，图片查看器会自动刷新）
 *
 * 主题（沿用 e1 的前缀，别和音频撞车）：
 *   <base>/cam/jpeg   板子 → PC，每帧一张 JPEG（二进制约 10~30KB）
 *   <base>/cam/cmd    PC → 板子，文本指令："SNAP" 抓一帧、"STREAM 1|0" 连续推、
 *                     "BAR 1|0" 开关内置彩条
 *
 * ============ 踩过的真 bug（都在这份代码里）============
 * 🔴 RESET 不能填 -1：这根线接 GPIO48，填 -1 就没人拉高它，模块一直复位。
 *    此时 VCC 3.3V、SDA/SCL 上拉 2.8V 都正常（上拉是模块自带的电阻，
 *    不依赖芯片工作），所以电气测量完全看不出问题 —— 填 48 后立刻读到 PID=0x76。
 * 🔴 内置彩条正常 ≠ 真实画面正常。彩条是芯片内部生成的数字图案，完全绕开
 *    "镜头→感光→ADC"这条模拟通路，所以它只能证明数据线/时序对。
 * 🔴 frame2jpg 会 free 掉传入的 buffer 再重新分配，不能缓存复用，每次传 NULL。
 * 🔴 Wi-Fi 要 nvs_flash_init()，否则 ESP_ERR_NVS_NOT_INITIALIZED 直接崩。
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "cam_reg.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sensor.h"

#include "secrets.h"       /* WIFI_SSID / WIFI_PASS / MQTT_USER / MQTT_PASS */

static const char *TAG = "e2cam";

/* ================= 引脚 =================
 * e1 已占：0(键) 4/5/6(麦) 7/15/16(功放) 41/42(屏)
 * 必须避开：33-37(八线 PSRAM) 43/44(CH340) 45/46(strapping) 19/20(原生USB)
 */
#define CAM_D0      8
#define CAM_D1      9
#define CAM_D2     10
#define CAM_D3     11
#define CAM_D4     12
#define CAM_D5     13
#define CAM_D6     14
#define CAM_D7     17

#define CAM_XCLK   18
#define CAM_PCLK   21
#define CAM_VSYNC  38
#define CAM_HREF   39

#define CAM_SIOC   40
#define CAM_SIOD    1

#define CAM_RESET  48      /* 必须填；填 -1 模块一直复位 */
#define CAM_PWDN   -1      /* 接 GND */

/* broker 与主题前缀（和 e1 保持一致，只换话题后缀） */
#define BROKER_URI   "mqtts://i1e78188.ala.cn-hangzhou.emqxsl.cn:8883"
#define MQTT_PORT    8883
#define TOPIC_BASE   "whale-8f2a5c31"

/* 预览尺寸：QVGA。OV7670 不产 JPEG，必须板子上转一次（约 300~800ms）。 */
#define PREVIEW_W   320
#define PREVIEW_H   240

static EventGroupHandle_t s_wifi_ev;
#define WIFI_OK_BIT  BIT0

static char s_topic_jpeg[96];
static char s_topic_cmd[96];

static volatile bool s_streaming;      /* 连续推流开关 */
static volatile bool s_snap_once;      /* 抓一张的请求 */

static camera_config_t make_config(void)
{
    camera_config_t c = {
        .pin_pwdn     = CAM_PWDN,
        .pin_reset    = CAM_RESET,
        .pin_xclk     = CAM_XCLK,
        .pin_sccb_sda = CAM_SIOD,
        .pin_sccb_scl = CAM_SIOC,

        .pin_d7 = CAM_D7, .pin_d6 = CAM_D6, .pin_d5 = CAM_D5, .pin_d4 = CAM_D4,
        .pin_d3 = CAM_D3, .pin_d2 = CAM_D2, .pin_d1 = CAM_D1, .pin_d0 = CAM_D0,

        .pin_vsync = CAM_VSYNC,
        .pin_href  = CAM_HREF,
        .pin_pclk  = CAM_PCLK,

        .xclk_freq_hz = 24000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_RGB565,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };
    return c;
}

/* ---------------- Wi-Fi ---------------- */
static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi 断开，重连…");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 通了，本机 IP = " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_ev, WIFI_OK_BIT);
    }
}

static bool wifi_start(void)
{
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_LOGI(TAG, "正在连 Wi-Fi: \"%s\" …", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_OK_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_OK_BIT)) {
        ESP_LOGE(TAG, "没连上 Wi-Fi（查名字/密码，只支持 2.4GHz）");
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

/* ---------------- MQTT ---------------- */
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_ok;
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    if (ev->event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt_ok = true;
        esp_mqtt_client_subscribe(s_mqtt, s_topic_cmd, 0);
        ESP_LOGE(TAG, "MQTT-OK 连上了（主题 %s）", TOPIC_BASE);
        ESP_LOGI(TAG, "  发 %s 抓一张 / %s 看实时画面", s_topic_cmd, s_topic_jpeg);
    } else if (ev->event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_ok = false;
        ESP_LOGW(TAG, "MQTT 断开");
    } else if (ev->event_id == MQTT_EVENT_DATA) {
        char cmd[32] = { 0 };
        int n = ev->data_len < (int)sizeof(cmd) - 1 ? ev->data_len : (int)sizeof(cmd) - 1;
        memcpy(cmd, ev->data, n);
        cmd[n] = 0;

        ESP_LOGI(TAG, "收到指令: %s", cmd);
        if (strncmp(cmd, "SNAP", 4) == 0) {
            s_snap_once = true;
        } else if (strncmp(cmd, "STREAM", 6) == 0) {
            s_streaming = (strstr(cmd, "1") != NULL);
            ESP_LOGI(TAG, "连续推流 => %s", s_streaming ? "开" : "关");
        } else if (strncmp(cmd, "BAR", 3) == 0) {
            sensor_t *s = esp_camera_sensor_get();
            bool on = (strstr(cmd, "1") != NULL);
            if (s && s->set_colorbar) s->set_colorbar(s, on ? 1 : 0);
            ESP_LOGI(TAG, "内置彩条 => %s", on ? "开" : "关");
        } else if (strncmp(cmd, "REG", 3) == 0) {
            /* REG 15 02      写一个寄存器
             * REG 15         读一个寄存器
             * DUMP           把关键寄存器全读一遍 */
            unsigned r = 0, v = 0;
            if (sscanf(cmd, "REG %x %x", &r, &v) == 2) {
                if (cam_reg_write((uint8_t)r, (uint8_t)v) == 0) {
                    ESP_LOGI(TAG, "写 %02X = %02X 成功", r, v);
                }
            } else if (sscanf(cmd, "REG %x", &r) == 1) {
                /* 只读 */
            } else {
                r = 0xFFFF;
            }
            if (r != 0xFFFF) {
                uint8_t got = 0;
                if (cam_reg_read((uint8_t)r, &got) == 0) {
                    ESP_LOGI(TAG, "回读 %02X = %02X", r, got);
                }
            }
        } else if (strncmp(cmd, "DUMP", 4) == 0) {
            /* 把几个关键寄存器读回来。这在本仓以前做不到 ——
             * esp32-camera 的 ov7670 驱动没实现 get_reg（NULL 指针）。 */
            static const uint8_t regs[] = {
                0x11, /* CLKRC   内部时钟分频 */
                0x12, /* COM7    输出格式 / 分辨率 */
                0x13, /* COM8    AGC/AEC/AWB 使能 */
                0x15, /* COM10   PCLK/HREF/VSYNC 极性 */
                0x17, 0x18, 0x32, /* HSTART/HSTOP/HREF */
                0x19, 0x1A, 0x03, /* VSTART/VSTOP/VREF */
                0x1E, /* MVFP    镜像/翻转 */
                0x3A, /* TSLB    输出顺序 */
                0x40, /* COM15   输出范围/RGB565 */
                0x70, 0x71, 0x72, 0x73, 0xA2, /* SCALING_* */
                0x0C, 0x3E, /* COM3 / COM14（DCW 缩放） */
                0x0D, /* COM4    PLL */
            };
            for (unsigned i = 0; i < sizeof(regs); i++) {
                uint8_t got = 0;
                if (cam_reg_read(regs[i], &got) == 0) {
                    ESP_LOGI(TAG, "  %02X = %02X", regs[i], got);
                }
            }
        }
    }
}

static void mqtt_start(void)
{
    snprintf(s_topic_jpeg, sizeof(s_topic_jpeg), "%s/cam/jpeg", TOPIC_BASE);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/cam/cmd", TOPIC_BASE);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BROKER_URI,
        .broker.address.port = MQTT_PORT,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .session.keepalive = 30,
        .buffer.size = 8192,
        .buffer.out_size = 65536,        /* 一帧 JPEG 可能 20KB+，出缓冲要够大 */
        .network.timeout_ms = 10000,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* ---------------- 抓帧转 JPEG ----------------
 * ⚠️ frame2jpg 对传入的 buf 会先 free 再重新 malloc，所以必须每次传 NULL。
 */
static uint8_t *grab_jpeg(size_t *len)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "fb_get 失败");
        return NULL;
    }
    uint8_t *buf = NULL;
    bool ok = frame2jpg(fb, 60, &buf, len);
    esp_camera_fb_return(fb);
    if (!ok || buf == NULL) {
        ESP_LOGE(TAG, "frame2jpg 失败");
        return NULL;
    }
    return buf;
}

/* ---------------- 串口命令通道 ----------------
 * 只插 USB 线就能调参：不用 MQTT、不用反复重烧。
 *   REG xx yy    写一个寄存器（并回读）
 *   REG xx       只读一个寄存器
 *   DUMP         把关键寄存器全读一遍
 *   MODE qvga|vga  切分辨率
 *   RATE msl     改推帧间隔（毫秒）
 *   HELP         打这份说明
 *
 * 为什么要它：调 OV7670 是在"改一个值看一眼"的循环里找出路，
 * 每次重烧要好几分钟，而这个循环要跑几十次。
 */
static volatile int s_uart_rate_ms = 120;   /* 921600 baud ≈ 92KB/s，一帧 ~12KB ⇒ 7fps 够用 */

static void uart_line(const char *cmd)
{
    unsigned r = 0, v = 0;

    if (strncmp(cmd, "REG", 3) == 0) {
        if (sscanf(cmd, "REG %x %x", &r, &v) == 2) {
            if (cam_reg_write((uint8_t)r, (uint8_t)v) == 0) {
                ESP_LOGW(TAG, "写 %02X = %02X", r, v);
            } else {
                ESP_LOGW(TAG, "写 %02X = %02X 失败", r, v);
            }
        } else {
            if (sscanf(cmd, "REG %x", &r) != 1) return;
        }
        uint8_t got = 0;
        if (cam_reg_read((uint8_t)r, &got) == 0) {
            ESP_LOGW(TAG, "回读 %02X = %02X", r, got);
        }
    } else if (strncmp(cmd, "DUMP", 4) == 0) {
        static const uint8_t regs[] = { 0x11, 0x12, 0x13, 0x15, 0x17, 0x18,
                                        0x32, 0x19, 0x1A, 0x03, 0x1E, 0x3A,
                                        0x40, 0x0C, 0x3E, 0x0D, 0x70, 0x71,
                                        0x72, 0x73, 0xA2, 0xB0, 0x6F };
        for (unsigned i = 0; i < sizeof(regs); i++) {
            uint8_t got = 0;
            if (cam_reg_read(regs[i], &got) == 0) {
                ESP_LOGW(TAG, "  %02X = %02X", regs[i], got);
            }
        }
    } else if (strncmp(cmd, "RATE", 4) == 0) {
        int ms = 0;
        if (sscanf(cmd, "RATE %d", &ms) == 1 && ms >= 0 && ms <= 5000) {
            s_uart_rate_ms = ms;
            ESP_LOGW(TAG, "推帧间隔 => %d ms", ms);
        }
    } else if (strncmp(cmd, "MODE", 4) == 0) {
        sensor_t *s = esp_camera_sensor_get();
        if (!s || !s->set_framesize) return;
        if (strstr(cmd, "vga")) {
            s->set_framesize(s, FRAMESIZE_VGA);
            ESP_LOGW(TAG, "分辨率 => VGA 640x480");
        } else {
            s->set_framesize(s, FRAMESIZE_QVGA);
            ESP_LOGW(TAG, "分辨率 => QVGA 320x240");
        }
    } else if (strncmp(cmd, "HELP", 4) == 0) {
        ESP_LOGW(TAG, "REG xx yy | REG xx | DUMP | MODE qvga|vga | RATE ms | HELP");
    }
}

static void uart_cmd_task(void *arg)
{
    char line[64];
    int n = 0;

    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = 0;
                uart_line(line);
                n = 0;
            }
            continue;
        }
        if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}


/* ---------------- UART 发 JPEG ----------------
 * 数据线直传，不依赖网络。PC 端用 tools/cam_grab.py 收。
 * 帧格式：<JPG START bytes=N>
  + N 个字节  + </JPG END>

 *
 * 为什么不用原来的裸 RGB：115200 下 QVGA 裸 RGB 要 13 秒，
 * 而 JPEG 只要 1~2 秒（体积差十几倍）。
 */
static void uart_send_jpeg(void)
{
    size_t len = 0;
    uint8_t *jpg = grab_jpeg(&len);
    if (!jpg) return;

    /* ⚠️ 关键：日志和 JPEG 共用 UART0。Wi-Fi/MQTT 任务一打印，
     * 就会把字节插进 JPEG 数据中间 —— 实测收到的图少 9 字节、没有 EOI(ffd9)，
     * PIL 报 "image file is truncated"。所以发送期间把日志整个关掉。 */
    esp_log_level_set("*", ESP_LOG_NONE);

    char head[48];
    int hn = snprintf(head, sizeof(head), "<JPG START bytes=%u>\n", (unsigned)len);
    fwrite(head, 1, hn, stdout);
    fwrite(jpg, 1, len, stdout);
    fwrite("</JPG END>\n", 1, 12, stdout);
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "UART 发出 %u 字节 JPEG", (unsigned)len);
    free(jpg);
}

/* 抓一帧并发布。返回是否成功。 */
static bool publish_one(void)
{
    if (!s_mqtt_ok) return false;

    size_t len = 0;
    uint8_t *jpg = grab_jpeg(&len);
    if (!jpg) return false;

    int id = esp_mqtt_client_publish(s_mqtt, s_topic_jpeg,
                                     (const char *)jpg, (int)len, 0, 0);
    free(jpg);

    if (id < 0) {
        ESP_LOGW(TAG, "publish 失败");
        return false;
    }
    ESP_LOGI(TAG, "发出 %u 字节 JPEG", (unsigned)len);
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== OV7670 → MQTT =====");
    ESP_LOGI(TAG, "D0..D7=%d %d %d %d %d %d %d %d", CAM_D0, CAM_D1, CAM_D2,
             CAM_D3, CAM_D4, CAM_D5, CAM_D6, CAM_D7);
    ESP_LOGI(TAG, "XCLK=%d PCLK=%d VSYNC=%d HREF=%d", CAM_XCLK, CAM_PCLK,
             CAM_VSYNC, CAM_HREF);
    ESP_LOGI(TAG, "SIOC=%d SIOD=%d RESET=%d PWDN=%d", CAM_SIOC, CAM_SIOD,
             CAM_RESET, CAM_PWDN);

    /* Wi-Fi 要靠 NVS 存配置，不 init 会 ESP_ERR_NVS_NOT_INITIALIZED */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nerr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nerr);

    /* RESET 先拉高 —— 漏掉这一步模块会一直复位 */
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << CAM_RESET,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level(CAM_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM 没起来，抓帧会很吃力");
    }

    camera_config_t cfg = make_config();
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init 失败 0x%x (%s)", err, esp_err_to_name(err));
        if (err == ESP_ERR_CAMERA_NOT_DETECTED || err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "  => 先查 RESET(GPIO%d) 有没有被拉高", CAM_RESET);
        }
        while (1) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "PID=0x%02X VER=0x%02X", s->id.PID, s->id.VER);
        if (s->set_colorbar) s->set_colorbar(s, 0);

        /* 打开寄存器直通（esp32-camera 的 ov7670 没实现 set_reg/get_reg，
         * 想调寄存器只能自己走 I2C）。失败不影响出图，只影响调试。 */
        cam_reg_init();

        /* 把初始化后的寄存器状态打一遍 —— 排查"每行只有一个采样值"
         * 需要知道芯片真正在什么模式下跑，而不是我们以为它是什么模式。 */
        static const uint8_t dump0[] = { 0x11, 0x12, 0x13, 0x15, 0x17, 0x18,
                                         0x32, 0x19, 0x1A, 0x03, 0x1E, 0x3A,
                                         0x40, 0x0C, 0x3E, 0x0D, 0x70, 0x71,
                                         0x72, 0x73, 0xA2 };
        for (unsigned i = 0; i < sizeof(dump0); i++) {
            uint8_t got = 0;
            if (cam_reg_read(dump0[i], &got) == 0) {
                ESP_LOGI(TAG, "  寄存器 %02X = %02X", dump0[i], got);
            }
        }

        /* BAND 试验：把 COM10 的 PCLK 改成"只在 HREF 有效期间输出"。
         * 现在 PCLK 是自由振荡（COM10_PCLK_FREE），若模块的 PCLK 与 HREF
         * 相对相位不对，行内就会整片错位 —— 症状正是"每行只有末尾几个像素对"。
         * 串口发 BAND 1 / BAND 0 现场切换，两种都能抓帧对比。 */
        /* COM10(0x15) bit5=1 → PCLK 只在 HREF 有效期间输出（banding），
         * bit5=0 → PCLK 自由振荡。两种都抓一次图对比，就能判断
         * "每行只有末尾几个像素对"是不是 PCLK/HREF 相位问题。 */
        /* ===== 决定性实验：固定曝光 + 固定低增益 =====
         * 逻辑：如果镜头没进光（盖着/贴膜），AGC 会把增益拉到最大，
         *   输出就是"每行一个随机灰度、行内一片平"的噪声 —— 正是现在看到的形状。
         * 关掉自动、把增益和曝光都压到最低：
         *   - 画面变成全黑  ⇒ 确实没有光进来（去查镜头盖/膜/镜头没拧开）
         *   - 画面还是花    ⇒ 光进去了，问题在数字时序/寄存器
         */
        /* 决定性实验的结论（2026-10-31）：
         * 固定 gain=0 / aec=8 后，画面均值从 147 掉到 64，横向差异从 13% 掉到 2%。
         * ⇒ 增益和曝光确实能改变亮度，说明"光→感光→ADC"这条通路是活的；
         *   但每一行内部仍然是同一个值 —— 也就是**有光进来，只是一行只给出一个采样**。
         * 现在恢复自动曝光/增益，做正常采集。 */
        /* ===== 2026-10-31 起：不写任何自定义寄存器 =====
         *
         * 依据 llcircle/esp32s3_ov7670_wifi（ESP32-S3 + 不带 FIFO 的 OV7670，
         * 走 esp32-camera 组件，README 明说"能出图，只是不太清晰"）。
         * 它的 main.c 里**一个寄存器都没写**，全靠驱动默认值。
         *
         * 我们之前几十轮把默认值改了一大堆（CLKRC/COM4/COM10/DCW/
         * frameControl...），每一处都"看着有道理"，但组合起来没人验证过
         * 整体是好是坏 —— 结果一路把基线改离了"能用"的状态。
         * 现在全部撤掉，回到驱动默认。以后再动寄存器，前提是先用默认值
         * 拿到一张可辨认的图当基线，并且一次只改一个字节、改完能回滚。
         *
         * 唯一的例外是 RESET：这块板的 RESET 接在 GPIO48 上，
         * pin_reset=-1 时没人驱动它，低有效就一直复位（表现为读不到 PID）。 */
        if (s->set_gain_ctrl)     s->set_gain_ctrl(s, 1);
        if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
        if (s->set_whitebal)      s->set_whitebal(s, 1);
    }

    /* 丢掉前 2 帧：实测第 1 帧是"没填满"的 */
    for (int k = 0; k < 2; k++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!wifi_start()) {
        ESP_LOGE(TAG, "Wi-Fi 失败");
        while (1) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    mqtt_start();

    /* 串口命令通道：只插 USB 线就能调寄存器（不依赖 MQTT） */
    xTaskCreate(uart_cmd_task, "uartcmd", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, ">>> 等 MQTT 连上后，电脑端发 SNAP 就能取一张 <<<");
    ESP_LOGI(TAG, "    tools/cam_view.py --loop   可以一直看");
    ESP_LOGI(TAG, "");

    int64_t last_stream_us = 0;
    int64_t last_uart_us = 0;

    while (1) {
        /* 收到 SNAP：抓一张走 MQTT */
        if (s_snap_once) {
            s_snap_once = false;
            publish_one();
        }

        /* 连续推流（MQTT）：最多 2 fps */
        if (s_streaming) {
            int64_t now = esp_timer_get_time();
            if (now - last_stream_us > 500000) {
                last_stream_us = now;
                publish_one();
            }
        }

        /* UART 一直推：默认 120ms 一帧（921600 baud ≈ 92KB/s）。
         * 不需要任何指令，插着 USB 线就能看。断网也能用 —— 最可靠的调试通路。
         * 想改节奏：串口发 `RATE 40`（更快）或 `RATE 1000`（更慢）。 */
        int64_t now2 = esp_timer_get_time();
        if (s_uart_rate_ms > 0 && now2 - last_uart_us > (int64_t)s_uart_rate_ms * 1000) {
            last_uart_us = now2;
            uart_send_jpeg();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
