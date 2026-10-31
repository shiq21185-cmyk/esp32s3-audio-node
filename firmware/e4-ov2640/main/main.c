/* e4-ov2640 —— OV2640 出图，走 UART 推 JPEG（接 OV7670 那一版的班）
 *
 * 为什么换：OV7670（不带 FIFO）在这块面包板上只能做到"看得出是张图"。
 *   QVGA 走驱动默认的 DCW 降采样会退化成一整行一个颜色，只能退到 VGA；
 *   再把 PCLK 压到 XCLK/3 才勉强连贯，噪点多、颜色脏，还固定有个横向断口。
 *   复盘写在 docs/OV2640-换型准备.md。
 *
 * OV2640 的两个决定性好处（esp32-camera README 明说）：
 *   1. 传感器内部直接出 JPEG —— VGA 一帧只有几十 KB，921600 波特率下
 *      0.3~0.6 秒传完。OV7670 是裸 RGB565，一帧 614400 字节要 6.7 秒，
 *      为了省带宽还被逼着改 PCLK。
 *   2. JPEG 模式下驱动自己把 PCLK 分频（pclk_div=8），对面包板 +
 *      杜邦线这种烂线最友好 —— 正好躲开 OV7670 上那个
 *      "PCLK 太快、ESP32 采到没稳定的数据"的坑。
 *
 * 接线和 e3 完全一样（同一个 DVP 并口 + SCCB），换模块不用换线：
 *   D0..D7 = 8,9,10,11,12,13,14,17   XCLK=18  PCLK=21
 *   VSYNC=38  HREF=39  SIOC=40  SIOD=1  RESET=48  PWDN=GND
 *
 * ⚠️ RESET 必须由芯片驱动（不能填 -1）：OV7670 上吃过一次亏 ——
 *    模块上没有上拉，悬空时芯片随机复位，寄存器读出来全是上电默认值，
 *    电气测量还一切正常。OV2640 的 RESET 也是低有效，同样交给 GPIO48。
 *
 * ⚠️ 不写任何自定义寄存器。OV2640 的驱动（sensors/ov2640.c）自带完整
 *    settings 表，JPEG/窗口/PCLK 分频都由 set_framesize 算好；手册上那些
 *    "看着有道理"的魔改，OV7670 那轮已经证明会把基线改废。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_camera.h"
#include "lwip/sockets.h"

/* wifi_stream.c：Wi-Fi 上的 MJPEG 服务（/stream /snap.jpg /stats /ctl） */
void wifi_stream_start(void);
const char *wifi_stream_ip(void);

/* net_push.c：板子主动把帧推给 PC（PC 在别的网段、连不进板子时的反向通道）
 * 默认关 —— 实测跨 NAT 只有 0.3~2.1fps，被 MQTT 那条（2~5fps）全面压过。 */
#define NET_PUSH_ENABLE 0
void net_push_start(void);
bool net_push_connected(void);

/* mqtt_cam.c：走 broker 推 JPEG（跨网看图，和 e2-camera 同一套协议） */
void mqtt_cam_start(void);
bool mqtt_cam_streaming(void);

/* 借 esp32-camera 自己的 SCCB 读写（调寄存器用）。
 * 绝不要 include driver/i2c_master.h —— 会和旧 driver 冲突，
 * 触发 "CONFLICT! driver_ng is not allowed to be used with this old driver"
 * 然后每秒重启一次（e3 上踩过）。 */
extern uint8_t SCCB_Read(uint8_t slv_addr, uint8_t reg);
extern int SCCB_Write(uint8_t slv_addr, uint8_t reg, uint8_t data);
#define CAM_SCCB_ADDR 0x21

/* ---------------- 引脚（和 e3 同一套线） ---------------- */
#define CAM_PIN_PWDN  -1   /* 模块 PWDN 直接接 GND（低 = 正常工作） */
#define CAM_PIN_RESET 48   /* 必须由芯片拉高，见文件头 */
#define CAM_PIN_XCLK  18
#define CAM_PIN_SIOD  1    /* SCCB SDA */
#define CAM_PIN_SIOC  40   /* SCCB SCL */

#define CAM_PIN_D7 17
#define CAM_PIN_D6 14
#define CAM_PIN_D5 13
#define CAM_PIN_D4 12
#define CAM_PIN_D3 11
#define CAM_PIN_D2 10
#define CAM_PIN_D1 9
#define CAM_PIN_D0 8

#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  39
#define CAM_PIN_PCLK  21   /* 模块丝印上叫 DCLK 的也是它（data clock = 像素时钟） */

/* 有些模块自带晶振，没有 XCLK/MCLK 输入脚 —— 那种情况下 ESP32 不能去驱动它：
 * 把这里改成 1，pin_xclk 置 -1，驱动会整段跳过 XCLK 输出（esp_camera.c:186
 * `if (config->pin_xclk >= 0)`），XCLK 那根线也就不接。
 * 普通模块（有 XCLK/MCLK 脚）保持 0。 */
#define E4_MODULE_HAS_OWN_OSC 1

static const char *TAG = "e4";

static camera_config_t camera_config = {
    .pin_pwdn     = CAM_PIN_PWDN,
    .pin_reset    = CAM_PIN_RESET,
    .pin_xclk     = E4_MODULE_HAS_OWN_OSC ? -1 : CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,

    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href  = CAM_PIN_HREF,
    .pin_pclk  = CAM_PIN_PCLK,

    /* OV2640 的标准时钟：乐鑫官方例程就是 20MHz。
     * 要是枚举不到 / 帧是花的，先降到 10MHz 再试 ——
     * 面包板上的杜邦线撑不住高频这件事，OV7670 已经教过一次。 */
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    /* JPEG 是关键：传感器硬件出图，不占 CPU 也不吃带宽。
     * 想要更大就改 FRAMESIZE_UXGA(1600x1200)，代价是 UART 要传 1 秒多。 */
    .pixel_format  = PIXFORMAT_JPEG,
#ifdef E4_QVGA
    .frame_size    = FRAMESIZE_QVGA,   /* -DE4_QVGA：跨云推流试验用 320x240 */
#else
    .frame_size    = FRAMESIZE_VGA,
#endif
    .jpeg_quality  = 12,          /* 0~63，数字越小越清晰 */
    .fb_count      = 2,           /* JPEG 才允许多缓冲（README 明确说了） */
    .grab_mode     = CAMERA_GRAB_LATEST,
    .jpeg_buffer_size = 0,        /* 0 = 按 frame_size 自动算 */
};

/* ---------------- 串口命令通道（调参用） ---------------- */
/*     REG rr [vv]   写 bank0 寄存器（并回读确认）；只给 rr 则读
 *     BANK xx       写 OV2640 的 bank 选择寄存器（0xFF）
 *     RATE 500      推帧间隔（毫秒）
 *     PUSH 1 / 0    开/关推帧（默认关，免得一插线就刷屏）
 *     HELP
 *
 * 默认不推帧：插上线先让启动日志打出来（PID、预热帧字节数），
 * 确认没问题再 PUSH 1 要图。 */
/* 帧间延时：默认 0（发完立刻取下一帧）。以前是 200ms，把帧率硬压到 3.4fps，
 * 调焦时看着像卡顿。调焦/看画质就该是全速，真要限速再用 RATE 命令。 */
static volatile int s_rate_ms = 0;
static volatile int s_push = 0;

/* NET <ip> <port>：从板子往外连一次 TCP，3 秒超时。
 * 用途：板子在自己网段里，PC 可能在别的网段 —— 由板子主动去连 PC，
 * 能连通就说明"PC 看不见板子"只是 NAT 方向问题，反向推流可行。 */
static void net_probe(const char *host, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        printf("[NET] 地址不合法：%s\n", host);
        return;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("[NET] socket() 失败\n");
        return;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    int64_t t0 = esp_timer_get_time();
    int rc = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (rc < 0 && errno != EINPROGRESS) {
        printf("[NET] %s:%d 立即失败 errno=%d\n", host, port, errno);
        close(s);
        return;
    }
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    rc = select(s + 1, NULL, &wf, NULL, &tv);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (rc == 0) {
        printf("[NET] %s:%d 超时 3000ms —— 不通\n", host, port);
    } else if (rc < 0) {
        printf("[NET] select 失败 errno=%d\n", errno);
    } else {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        printf("[NET] %s:%d %s（%dms）\n", host, port, err == 0 ? "通" : "被拒绝/不通", ms);
    }
    close(s);
}

static void uart_line(char *cmd)
{
    unsigned r = 0, v = 0;    if (!strncmp(cmd, "REG", 3)) {
        if (sscanf(cmd + 3, "%x %x", &r, &v) == 2) {
            SCCB_Write(CAM_SCCB_ADDR, (uint8_t)r, (uint8_t)v);
            printf("[REG] %02X := %02X  回读 %02X\n", r, v,
                   SCCB_Read(CAM_SCCB_ADDR, (uint8_t)r));
        } else if (sscanf(cmd + 3, "%x", &r) == 1) {
            printf("[REG] %02X = %02X\n", r, SCCB_Read(CAM_SCCB_ADDR, (uint8_t)r));
        }
    } else if (!strncmp(cmd, "BANK", 4)) {
        if (sscanf(cmd + 4, "%x", &v) == 1) {
            SCCB_Write(CAM_SCCB_ADDR, 0xFF, (uint8_t)v);
            printf("[BANK] := %02X  回读 %02X\n", v, SCCB_Read(CAM_SCCB_ADDR, 0xFF));
        }
    } else if (!strncmp(cmd, "RATE", 4)) {
        int ms = atoi(cmd + 4);
        if (ms >= 0) { s_rate_ms = ms; printf("[RATE] %d ms\n", ms); }
    } else if (!strncmp(cmd, "PUSH", 4)) {
        s_push = (atoi(cmd + 4) != 0);
        printf("[PUSH] %s\n", s_push ? "开" : "关");
    } else if (!strncmp(cmd, "NET", 3)) {
        char host[40];
        int port = 0;
        if (sscanf(cmd + 3, "%39s %d", host, &port) == 2 && port > 0) {
            printf("[NET] 本机 %s，正在连 %s:%d ...\n", wifi_stream_ip(), host, port);
            net_probe(host, port);
        } else {
            printf("用法：NET 192.168.1.10 8765\n");
        }
    } else if (!strncmp(cmd, "HELP", 4)) {
        printf("REG rr [vv] | BANK xx | RATE ms | PUSH 0|1 | NET ip port | HELP\n");
    }
}

static void cmd_task(void *arg)
{
    char line[64];
    int n = 0;
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') {
            if (n > 0) { line[n] = 0; uart_line(line); n = 0; }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

/* ---------------- UART 发帧 ---------------- */
/* 帧格式（和 e3 同一个外壳，数据从 RGB565 换成 JPEG）：
 *     META,JPEG,<字节数>\n
 *     <JPEG 原始字节>
 *     \nEND\n
 * 电脑端读第一行拿到长度，再精确读那么多字节，直接存 .jpg 就能看。
 *
 * 发数据期间必须闭掉日志：ESP_LOGI 和图像共用 UART0，
 * 别的任务一打印就会把字节插进图像中间（e2 上踩过）。 */
static void uart_send_jpeg(const uint8_t *buf, size_t len)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("META,JPEG,%u\n", (unsigned)len);
    fwrite(buf, 1, len, stdout);
    printf("\nEND\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "===== OV2640 -> UART(JPEG) =====");
    ESP_LOGI(TAG, "引脚 XCLK=%s SDA=%d SCL=%d D0..D7=%d/%d/%d/%d/%d/%d/%d/%d "
                  "VSYNC=%d HREF=%d PCLK(DCLK)=%d RESET=%d",
             camera_config.pin_xclk < 0 ? "不用（模块自带晶振）" : "18",
             CAM_PIN_SIOD, CAM_PIN_SIOC,
             CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
             CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
             CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK, CAM_PIN_RESET);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: 0x%x（先查 VCC/GND、SDA/SCL、XCLK、RESET）", err);
        ESP_LOGE(TAG, "  0x105 = 没检测到传感器（SCCB 读不到 PID）");
        ESP_LOGE(TAG, "  0x106 = 检测到但不支持（PID 不对 / 模块不是 OV 系）");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "检测到摄像头 PID=0x%02X VER=0x%02X MIDH=0x%02X MIDL=0x%02X",
                 s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
    }
    ESP_LOGI(TAG, "配置：XCLK=%dHz VGA JPEG(q=%d) fb_count=2",
             camera_config.xclk_freq_hz, camera_config.jpeg_quality);

    /* 丢掉前 2 帧：实测第 1 帧常"没填满"（AE/AWB 还没收敛） */
    for (int k = 0; k < 2; k++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) {
            ESP_LOGI(TAG, "预热第%d帧：%ux%u JPEG %u 字节", k,
                     warm->width, warm->height, (unsigned)warm->len);
            esp_camera_fb_return(warm);
        } else {
            ESP_LOGW(TAG, "预热第%d帧取帧失败", k);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    xTaskCreate(cmd_task, "cmd", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "就绪。推帧默认关：发 PUSH 1 出图。命令 REG/BANK/RATE/PUSH/HELP");

    /* 串口和 Wi-Fi 两条出图路都在：串口给命令行/无网场合，Wi-Fi 给实时预览。
     * 两边同时要图会互相丢帧（fb_count=2），调画质时只用 Wi-Fi 那条。 */
    wifi_stream_start();
    /* 反向推流（板子 -> PC 的 tools/cam_wifi.py）**默认关**：
     * 2026-10-31 实测它跨 NAT 只有 0.3~2.1fps，比 MQTT 的 2~5fps 还差，
     * 而且每 8 秒刷一行 "push: 连不上"，纯噪音。
     * 想再试：把 NET_PUSH_ENABLE 改成 1（代码留在 net_push.c 没删）。 */
#if NET_PUSH_ENABLE
    net_push_start();
#endif
    /* MQTT 由 wifi_stream.c 在拿到 IP 后启动（见 on_wifi_event）：
     * 提前起会 transport connect 失败再等退避，白慢十几秒。 */

    while (1) {
        /* 反向推流 / MQTT 推流连着时由各自的任务取帧，这里不抢 */
        if (net_push_connected() || mqtt_cam_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!s_push) {
            /* 不推帧也要继续取帧：DMA 不能停，AE/AGC 也要持续收敛，
             * 否则一开推帧那几帧是黑的。 */
            camera_fb_t *idle = esp_camera_fb_get();
            if (idle) esp_camera_fb_return(idle);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "取帧失败");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        uart_send_jpeg(fb->buf, fb->len);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(s_rate_ms));
    }
}
