/*
 * d3-net —— Wi-Fi 联网 + 麦克风音频 UDP 回传
 *
 * 干什么：
 *   1. 板子上电连你家 Wi-Fi（station 模式，就是"当客户端去连路由器"）
 *   2. 连上后串口打印拿到的 IP 地址
 *   3. 监听 UDP 5005，等电脑发来 "HELLO"
 *      —— 板子从这条消息里学到电脑的 IP 和端口，不写死
 *   4. 之后每 20ms 从 INMP441 读一块声音，原样（16bit 立体声）发回电脑
 *   5. 电脑发 "STOP" 就停，发 "PING" 回 "PONG"
 *
 * 为什么让电脑先说话：板子不知道电脑 IP，硬编码换台电脑就废了。
 * 让电脑先打一个招呼，板子记下是谁问的，后面就往那儿发。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "driver/i2s_std.h"

#include "secrets.h"

static const char *TAG = "d3";

/* ---------- 引脚：来自这块板子的实测（见 BODY-BUILD.md §3.2.1） ---------- */
#define PIN_MIC_SCK 4
#define PIN_MIC_WS  5
#define PIN_MIC_SD  6

#define SAMPLE_RATE   16000
#define CHUNK_FRAMES  320 /* 20ms @ 16kHz */
#define CHUNK_BYTES   (CHUNK_FRAMES * 2 * 2) /* 立体声 16bit => 1280 字节 */

#define UDP_PORT      5005
#define SOCK_TIMEOUT_MS 10

/* ---------- Wi-Fi 连接 ---------- */
static EventGroupHandle_t s_net_ev;
#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1

static int s_retry;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_retry < 10) {
            s_retry++;
            ESP_LOGW(TAG, "断开 (reason=%d)，第 %d 次重连…", d->reason, s_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "连了 10 次都失败，放弃 (reason=%d)", d->reason);
            xEventGroupSetBits(s_net_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "==================================");
        ESP_LOGI(TAG, "  连上 Wi-Fi 了！板子 IP = " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "==================================");
        s_retry = 0;
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

    EventBits_t bits = xEventGroupWaitBits(s_net_ev, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & BIT_CONNECTED) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "没连上。常见原因：");
    ESP_LOGE(TAG, "  - 名字/密码打错（注意大小写）");
    ESP_LOGE(TAG, "  - 这是 5GHz 网络，ESP32-S3 只认 2.4GHz");
    ESP_LOGE(TAG, "  - 路由器开了 WPA3，改成 WPA2/WPA2+WPA3 混合");
    return ESP_FAIL;
}

/* ---------- I2S 麦克风 ---------- */
static i2s_chan_handle_t s_rx;

static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_SD,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    return ESP_OK;
}

/* ---------- 主循环：UDP ---------- */
static void udp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket 创建失败");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_PORT),
    };
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "bind %d 失败", UDP_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = SOCK_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "UDP 已监听 %d，等电脑发 HELLO…", UDP_PORT);

    static uint8_t chunk[CHUNK_BYTES];
    char cmd[32];

    struct sockaddr_in peer = { 0 };
    bool have_peer = false;
    bool streaming = false;
    uint32_t sent = 0;
    TickType_t last_report = xTaskGetTickCount();

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, cmd, sizeof(cmd) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);

        if (n > 0) {
            cmd[n] = '\0';
            /* 去掉尾部换行 */
            for (int i = 0; i < n; i++) {
                if (cmd[i] == '\r' || cmd[i] == '\n') { cmd[i] = '\0'; break; }
            }
            ESP_LOGI(TAG, "收到命令: \"%s\"  来自 %s:%d", cmd,
                     inet_ntoa(from.sin_addr), ntohs(from.sin_port));

            if (strcmp(cmd, "HELLO") == 0) {
                peer = from;
                have_peer = true;
                streaming = true;
                sent = 0;
                const char *ack = "READY";
                sendto(sock, ack, strlen(ack), 0, (struct sockaddr *)&peer, sizeof(peer));
                ESP_LOGI(TAG, "开始推流 -> %s:%d", inet_ntoa(peer.sin_addr),
                         ntohs(peer.sin_port));
            } else if (strcmp(cmd, "STOP") == 0) {
                streaming = false;
                ESP_LOGI(TAG, "停止推流（共发 %lu 包）", (unsigned long)sent);
            } else if (strcmp(cmd, "PING") == 0) {
                const char *pong = "PONG";
                sendto(sock, pong, strlen(pong), 0, (struct sockaddr *)&from, sizeof(from));
            }
            continue;
        }

        if (!streaming || !have_peer) {
            continue;
        }

        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, chunk, CHUNK_BYTES, &got, 100);
        if (err != ESP_OK || got == 0) {
            continue;
        }

        sendto(sock, chunk, got, 0, (struct sockaddr *)&peer, sizeof(peer));
        sent++;

        if ((xTaskGetTickCount() - last_report) > pdMS_TO_TICKS(2000)) {
            ESP_LOGI(TAG, "推流中… 已发 %lu 包 (%lu KB)",
                     (unsigned long)sent, (unsigned long)(sent * got / 1024));
            last_report = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "\n===== d3-net: Wi-Fi + UDP 音频回传 =====");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "网络没通，先解决这个再往下");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    ESP_ERROR_CHECK(mic_init());
    ESP_LOGI(TAG, "麦克风就绪 (SCK=%d WS=%d SD=%d, %d Hz)",
             PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD, SAMPLE_RATE);

    xTaskCreate(udp_task, "udp", 8192, NULL, 5, NULL);
}
