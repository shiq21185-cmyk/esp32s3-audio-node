/* e3-ref —— 原样照抄 llcircle/esp32s3_ov7670_wifi 的实现，只为验证"这套接线能不能出图"。
 *
 * 来源：https://github.com/llcircle/esp32s3_ov7670_wifi
 *   .tmp/ref/llcircle_esp32s3_ov7670_wifi/ov7670_wifi/main/main.c（348 行）
 * 它是 ESP32-S3 + 不带 FIFO 的 OV7670，走乐鑫官方 esp32-camera 组件，
 * README 里明说"输出图像不是很清晰，感觉是正常的"——意思是它确实能出图。
 *
 * 和原版只差三处（都在下面标了 [改]）：
 *   1. 引脚换成我们这块面包板的（原版 XCLK=16/SIOD=5/SIOC=4，我们是 18/1/40…）
 *   2. Wi-Fi 从 SoftAP 改成 STA（我们要连家里路由器，不要热点）
 *   3. 传输从 TCP 改成 UART（用户明确要求"先走数据线"）
 *
 * ⚠️ 刻意保留原版的做法：**一个自定义寄存器都不写**，全用驱动默认。
 *    我们自己的 e2-camera 里堆了几十轮手工寄存器，最后把基线改离了可用状态。
 *    先让这份"干净的"跑出图，再谈优化。
 *
 * ⚠️ 唯一的例外是 RESET：这块板 RESET 接在 GPIO48 上。原版写 pin_reset=-1，
 *    因为他的模块 RESET 是接 3.3V 的。我们这里必须由芯片驱动这个脚，
 *    否则它一直处于复位电平（低有效），表现是 I2C 读不到 PID。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_camera.h"

/* 直接借 esp32-camera 自己的 SCCB 读写寄存器。
 * 为什么需要：esp32-camera 的 ov7670 驱动**没实现** set_reg/get_reg
 * （sensors/ov7670.c 里根本没给这两个函数指针赋值，是 NULL），
 * 所以想动一个字节只能自己走 I2C。绝不能 include driver/i2c_master.h，
 * 那会触发 "CONFLICT! driver_ng is not allowed to be used with this old driver"
 * 然后每秒重启。 */
extern uint8_t SCCB_Read(uint8_t slv_addr, uint8_t reg);
extern int SCCB_Write(uint8_t slv_addr, uint8_t reg, uint8_t data);
#define CAM_SCCB_ADDR 0x21

#include "secrets.h"

/* ---------------- 引脚：我们这块面包板 [改] ---------------- */
#define CAM_PIN_PWDN  -1   /* 用户 2026-10-31 把 PWDN 拔了（悬空） */
#define CAM_PIN_RESET 48   /* 必须由芯片拉高：这根线接 GPIO48，模块上没上拉，
                            * 填 -1 时它悬空乱跳（时好时坏），表现就是"有时能读到 PID、
                            * 有时 Detected camera not supported"，读出来的寄存器
                            * 也全是上电默认值。*/
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

/* VSYNC/HREF 互换试过一次：一帧都收不到 ⇒ 没接反，已换回。 */
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  39
#define CAM_PIN_PCLK  21

#define WIDTH  640
#define HEIGHT 480
#define ROW_SZ (WIDTH * 2)   /* 一行 1280 字节 */

static const char *TAG = "e3ref";

static camera_config_t camera_config = {
    .pin_pwdn     = CAM_PIN_PWDN,
    .pin_reset    = CAM_PIN_RESET,
    .pin_xclk     = CAM_PIN_XCLK,
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

    .xclk_freq_hz = 12000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_RGB565,
    .frame_size   = FRAMESIZE_VGA,

    .jpeg_quality = 12,
    .fb_count     = 2,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ---------------- Wi-Fi STA [改] ---------------- */
static EventGroupHandle_t s_wifi_ev;
#define WIFI_OK_BIT BIT0

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 通了，IP = " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_ev, WIFI_OK_BIT);
    }
}

static bool wifi_start(void)
{
    s_wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "正在连 Wi-Fi: \"%s\" …", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev, WIFI_OK_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_OK_BIT)) {
        ESP_LOGE(TAG, "没连上 Wi-Fi（查名字/密码，只支持 2.4GHz）");
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 连上以后停掉 Wi-Fi：这一版走数据线，不需要网络，开着只是白烧电、
     * 而且射频噪声对这块面包板上的并口线没好处。 */
    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi 已停（本版走 UART，不需要联网）");
    return true;
}

/* ---------------- 串口命令通道（调参用） ---------------- */
/* 一次烧录就能扫所有寄存器，不用每改一个字节重烧一遍。
 * 命令（一行一条，回车结束）：
 *     REG 07 00     写寄存器（并回读确认）
 *     REG 10        只读
 *     DUMP          把关键寄存器全读一遍
 *     RATE 500      改推帧间隔（毫秒）
 *     PUSH 1 / 0    开/关推帧（**默认关**）
 *     HELP
 *
 * 为什么默认不推帧：一帧 VGA RGB565 是 614400 字节，在 921600 波特率下
 * 要传 6.7 秒。这 6.7 秒里 UART 发送队列全被图像占着，命令的回应根本
 * 挤不出去 —— 实测发一条 REG 回应要等十几秒，调参完全没法做。
 * 所以改成"要图才推"：PUSH 1 之后才出图，PUSH 0 立刻停。
 *
 * ⚠️ 绝不能写 COM7(0x12) 的 bit7（全寄存器软复位）—— 驱动不会重新配置，
 *    传感器会停在未初始化状态，之后所有抓帧实验都被污染，只能重烧恢复。 */
static volatile int s_rate_ms = 300;
static volatile int s_push = 0;

static void uart_line(char *cmd)
{
    unsigned r = 0, v = 0;
    if (!strncmp(cmd, "REG", 3)) {
        if (sscanf(cmd + 3, "%x %x", &r, &v) == 2) {
            SCCB_Write(CAM_SCCB_ADDR, (uint8_t)r, (uint8_t)v);
            uint8_t got = SCCB_Read(CAM_SCCB_ADDR, (uint8_t)r);
            printf("[REG] %02X := %02X  回读 %02X\n", r, v, got);
        } else if (sscanf(cmd + 3, "%x", &r) == 1) {
            printf("[REG] %02X = %02X\n", r, SCCB_Read(CAM_SCCB_ADDR, (uint8_t)r));
        }
    } else if (!strncmp(cmd, "DUMP", 4)) {
        static const uint8_t regs[] = { 0x11,0x12,0x13,0x15,0x17,0x18,0x32,
                                        0x19,0x1A,0x03,0x1E,0x3A,0x40,
                                        0x04,0x07,0x10,0x00,0x14,0x0C,0x3E,0x0D };
        for (unsigned i = 0; i < sizeof(regs); i++)
            printf("[DUMP] %02X = %02X\n", regs[i], SCCB_Read(CAM_SCCB_ADDR, regs[i]));
    } else if (!strncmp(cmd, "RATE", 4)) {
        int ms = atoi(cmd + 4);
        if (ms > 0) { s_rate_ms = ms; printf("[RATE] %d ms\n", ms); }
    } else if (!strncmp(cmd, "PUSH", 4)) {
        s_push = (atoi(cmd + 4) != 0);
        printf("[PUSH] %s\n", s_push ? "开" : "关");
    } else if (!strncmp(cmd, "HELP", 4)) {
        printf("REG xx [yy] | DUMP | RATE ms | PUSH 0|1 | HELP\n");
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

/* ---------------- UART 发帧 [改] ---------------- */
/* 帧格式照原版的 TCP 版：
 *     META,<行字节数>,<宽>,<高>\n
 *     <裸 RGB565 数据，每像素 2 字节，高字节在前>
 *     \nEND\n
 * 用 printf 而不是 UART 驱动，是因为 CONFIG_NEWLIB_STDOUT 已经指向 UART0，
 * 而本工程 sdkconfig 里已关掉 CRLF 转换（否则 0x0A 会被加 \r）。 */
static void uart_send_frame(const uint8_t *buf, size_t len)
{
    /* 发数据期间闭掉日志：ESP_LOGI 和图像共用 UART0，
     * 别的任务一打印就会把字节插进图像中间（这个坑在 e2 里踩过）。 */
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("META,%d,%d,%d\n", ROW_SZ, WIDTH, HEIGHT);
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

    /* 相机先初始化：不依赖网络，网络挂了也照样能看出图不上 */
    ESP_LOGI(TAG, "初始化摄像头（照抄 llcircle：不写任何自定义寄存器）…");
    ESP_LOGI(TAG, "引脚 XCLK=%d SDA=%d SCL=%d D0..D7=%d/%d/%d/%d/%d/%d/%d/%d "
                  "VSYNC=%d HREF=%d PCLK=%d RESET=%d",
             CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC,
             CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2, CAM_PIN_D3,
             CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
             CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK, CAM_PIN_RESET);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败: 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "检测到摄像头 PID=0x%02X VER=0x%02X MIDH=0x%02X MIDL=0x%02X",
                 s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
    }
    ESP_LOGI(TAG, "配置：XCLK=%dHz 尺寸=%dx%d RGB565 fb_count=2 grab=WHEN_EMPTY",
             camera_config.xclk_freq_hz, WIDTH, HEIGHT);

    /* ===== 唯一的自定义寄存器：COM10(0x15) =====
     *
     * 驱动默认写 0x02（COM10_VSYNC_NEG），把 VSYNC 反相了。
     * 后果：每行像素本身都对（行内 lag1 = 0.95），但"哪一行算第 0 行"
     * 每帧都不一样 —— 帧间相关只有 0.1~0.2，看上去像满屏噪声。
     *
     * 实测（同一块板、同一套接线、判据=帧间归一化相关，真图像应 >0.8）：
     *   COM10=0x02（驱动默认）  0.463
     *   COM10=0x12（PCLK 反相） 0.874
     *   COM10=0x00（极性全自由） 0.987   ← 真图像的水平
     *   COM10=0x22 / 0x32（PCLK 门控）一帧都不出
     *   COM10=0x0A（HREF 反相）  全黑
     *
     * 为什么 llcircle 的代码在他那里不用改而我们这里必须改：
     * 同一型号的 OV7670 模块不同批次，VSYNC 的有效电平和上拉不一样。
     * 他 README 里那句"sda 和 scl 可能要反接"说的就是这类批次差异。
     *
     * ⚠️ 这是这一版**唯一**破例写的一个寄存器。其余全部保持驱动默认。 */
    {
        uint8_t before = SCCB_Read(CAM_SCCB_ADDR, 0x15);
        SCCB_Write(CAM_SCCB_ADDR, 0x15, 0x00);
        uint8_t after = SCCB_Read(CAM_SCCB_ADDR, 0x15);
        ESP_LOGI(TAG, "COM10(0x15): %02X -> %02X（极性全自由）", before, after);
    }

    /* ===== 第二个自定义寄存器：CLKRC(0x11) = 0x02 —— 把 PCLK 降慢 =====
     *
     * 起因（2026-10-31）：QVGA 那条路（驱动用 DCW 降采样）永远只给出
     * "每行一个颜色"的条带；换成 VGA（DCW 关掉）后终于出真图像，
     * 但满屏彩色雪花。雪花量随 PCLK 变慢单调下降，说明不是图像脏，
     * 是 ESP32 在 PCLK 上升沿采样时数据还没稳定 —— 面包板 + 杜邦线
     * 的容抗/串阻撑不住 12MHz。
     *
     * 同一场景实测（相邻像素平均绝对差，越小越干净）：
     *   CLKRC=0x00 (PCLK=XCLK    ) 1684
     *   CLKRC=0x01 (PCLK=XCLK/2  ) 1346
     *   CLKRC=0x02 (PCLK=XCLK/3  )  535   ← 用这个
     *   CLKRC=0x03 (PCLK=XCLK/4  )  705
     * 0x02 时帧间相关 0.93（真图像水平），画面从此连贯可辨认。
     */
    {
        uint8_t before = SCCB_Read(CAM_SCCB_ADDR, 0x11);
        SCCB_Write(CAM_SCCB_ADDR, 0x11, 0x02);
        uint8_t after = SCCB_Read(CAM_SCCB_ADDR, 0x11);
        ESP_LOGI(TAG, "CLKRC(0x11): %02X -> %02X（PCLK 降到 XCLK/3）", before, after);
    }

    /* 丢掉前 2 帧：实测第 1 帧没填满 */
    for (int k = 0; k < 2; k++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xTaskCreate(cmd_task, "cmd", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "预热完成。推帧默认关（发 PUSH 1 才出图）。命令：REG/DUMP/RATE/PUSH/HELP");

    /* Wi-Fi 只用来打一行 IP 方便确认板子活着；本版图像不走网络 */
    wifi_start();

    while (1) {
        if (!s_push) {
            /* 不推帧时也必须继续取帧并丢弃：I2S/LCD_CAM 的 DMA 不能停，
             * 而且要让 AEC/AGC 环路持续工作，否则一开推帧前几帧是黑的。 */
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
        uart_send_frame(fb->buf, fb->len);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(s_rate_ms));
    }
}
