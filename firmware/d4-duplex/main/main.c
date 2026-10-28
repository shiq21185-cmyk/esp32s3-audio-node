/*
 * d4-duplex —— 双向音频：麦克风上行 + 喇叭下行
 *
 * D3 只做了"板子 → 电脑"（麦克风推流）。
 * D4 加反向：电脑把 PCM 发给板子，板子通过 MAX98357 放出来。
 * 两条路同时通 ⇒ 一句话能完整往返，这就是"从身体里说话"的第一步。
 *
 * 和 D3 的两处关键改动：
 *   1. 喇叭有个独立的播放任务，**永远在往 I2S 里灌数据**：
 *      有声音就喂声音，没声音就喂零。
 *      这样彻底绕开 D2 那个"断流后 DMA 循环重放"的坑 —— 根本不是"停"，是"一直喂"。
 *   2. UDP 包用第一个字节区分类型：0xAA = 音频，其他 = 文本命令。
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "driver/i2s_std.h"

#include "secrets.h"

static const char *TAG = "d4";

/* ---------- 引脚：这块板的实测结果（BODY-BUILD.md §3.2.1） ---------- */
#define PIN_MIC_SCK 4
#define PIN_MIC_WS  5
#define PIN_MIC_SD  6

#define PIN_AMP_BCLK 16
#define PIN_AMP_LRC  17
#define PIN_AMP_DIN  15

#define SAMPLE_RATE   16000
#define CHUNK_FRAMES  320                      /* 20ms @ 16kHz */
#define CHUNK_BYTES   (CHUNK_FRAMES * 4)       /* 立体声 16bit => 1280 字节 */

#define UDP_PORT      5005
#define SOCK_TIMEOUT_MS 5

#define AUDIO_MAGIC   0xAA
#define SPK_BUF_SIZE  (48 * 1024)

/* ---------- Wi-Fi ---------- */
static EventGroupHandle_t s_net_ev;
#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1
static int s_retry;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
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
            ESP_LOGE(TAG, "连了 10 次都失败，放弃");
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
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "没连上。查：名字/密码、是不是 5GHz、路由器有没有开 WPA3");
        return ESP_FAIL;
    }

    /* 双向音频要低延迟，关掉 Wi-Fi 省电（会让 ping 从 70ms 掉到个位数） */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* ---------- I2S ---------- */
static i2s_chan_handle_t s_rx; /* 麦克风 */
static i2s_chan_handle_t s_tx; /* 喇叭 */
static StreamBufferHandle_t s_spk;

static esp_err_t audio_init(void)
{
    /* 麦克风 */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_MIC_SCK, .ws = PIN_MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = PIN_MIC_SD },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    /* 喇叭 */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_cfg.auto_clear = true; /* DMA 缓冲自动清零，双保险 */
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_AMP_BCLK, .ws = PIN_AMP_LRC,
                      .dout = PIN_AMP_DIN, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    s_spk = xStreamBufferCreate(SPK_BUF_SIZE, CHUNK_BYTES / 4);
    return ESP_OK;
}

/* ---------- 播放任务：永远在喂数据 ---------- */
static void spk_task(void *arg)
{
    static int16_t buf[CHUNK_FRAMES * 2];
    static int16_t zero[CHUNK_FRAMES * 2];

    while (1) {
        size_t total = xStreamBufferReceive(s_spk, buf, sizeof(buf), pdMS_TO_TICKS(20));

        if (total > 0) {
            /* 把缓冲区里剩下的都倒出来，凑满一个 20ms 块 */
            while (total < sizeof(buf)) {
                size_t n = xStreamBufferReceive(s_spk, (uint8_t *)buf + total,
                                                sizeof(buf) - total, 0);
                if (n == 0) break;
                total += n;
            }
            total = (total / 4) * 4;
            if (total < sizeof(buf)) {
                memset((uint8_t *)buf + total, 0, sizeof(buf) - total);
            }
        } else {
            /* 没数据就喂零 —— 不是"停"，是"一直喂"，DMA 才不会循环重放 */
            memcpy(buf, zero, sizeof(buf));
            memset(buf, 0, sizeof(buf));
        }

        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

/* ---------- 主循环：UDP ---------- */
static void udp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket 失败"); vTaskDelete(NULL); return; }

    struct sockaddr_in me = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(UDP_PORT),
    };
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "bind %d 失败", UDP_PORT);
        close(sock); vTaskDelete(NULL); return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = SOCK_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "UDP 已监听 %d（0xAA 开头=音频，其余=命令）", UDP_PORT);

    static uint8_t chunk[CHUNK_BYTES];
    static uint8_t rxpkt[2048];
    char cmd[32];

    struct sockaddr_in peer = { 0 };
    bool have_peer = false, streaming = false;
    uint32_t sent = 0, played = 0, dropped = 0;
    TickType_t last_report = xTaskGetTickCount();

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, rxpkt, sizeof(rxpkt), 0, (struct sockaddr *)&from, &fromlen);

        if (n > 0) {
            if (rxpkt[0] == AUDIO_MAGIC && n > 1) {
                /* 音频包：塞进播放缓冲 */
                size_t off = 1;
                size_t len = (size_t)(n - 1);
                len &= ~3u; /* 只收整帧 */
                if (len > 0) {
                    if (xStreamBufferSend(s_spk, rxpkt + off, len, 0) != len) {
                        dropped++;
                    }
                    played++;
                }
                continue;
            }

            memcpy(cmd, rxpkt, n < (int)sizeof(cmd) - 1 ? n : (int)sizeof(cmd) - 1);
            cmd[n < (int)sizeof(cmd) - 1 ? n : (int)sizeof(cmd) - 1] = '\0';
            for (int i = 0; i < n; i++) {
                if (cmd[i] == '\r' || cmd[i] == '\n') { cmd[i] = '\0'; break; }
            }
            ESP_LOGI(TAG, "命令 \"%s\" 来自 %s:%d", cmd, inet_ntoa(from.sin_addr), ntohs(from.sin_port));

            if (strcmp(cmd, "HELLO") == 0) {
                peer = from; have_peer = true; streaming = true; sent = 0;
                const char *ack = "READY";
                sendto(sock, ack, strlen(ack), 0, (struct sockaddr *)&peer, sizeof(peer));
                ESP_LOGI(TAG, "开始上行推流 -> %s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            } else if (strcmp(cmd, "STOP") == 0) {
                streaming = false;
                ESP_LOGI(TAG, "停止上行（发 %lu 包，播 %lu 包，丢 %lu）",
                         (unsigned long)sent, (unsigned long)played, (unsigned long)dropped);
            } else if (strcmp(cmd, "PING") == 0) {
                const char *pong = "PONG";
                sendto(sock, pong, strlen(pong), 0, (struct sockaddr *)&from, sizeof(from));
            }
            continue;
        }

        if (!streaming || !have_peer) continue;

        size_t got = 0;
        if (i2s_channel_read(s_rx, chunk, CHUNK_BYTES, &got, 100) != ESP_OK || got == 0) continue;
        sendto(sock, chunk, got, 0, (struct sockaddr *)&peer, sizeof(peer));
        sent++;

        if ((xTaskGetTickCount() - last_report) > pdMS_TO_TICKS(3000)) {
            ESP_LOGI(TAG, "上行 %lu 包 | 下行播 %lu 包 | 丢 %lu",
                     (unsigned long)sent, (unsigned long)played, (unsigned long)dropped);
            last_report = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "\n===== d4-duplex: 双向音频 =====");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (wifi_connect() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    ESP_ERROR_CHECK(audio_init());
    ESP_LOGI(TAG, "麦克风 SCK/WS/SD = %d/%d/%d，喇叭 BCLK/LRC/DIN = %d/%d/%d，%d Hz",
             PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD,
             PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN, SAMPLE_RATE);

    xTaskCreate(spk_task, "spk", 4096, NULL, 6, NULL);
    xTaskCreate(udp_task, "udp", 8192, NULL, 5, NULL);
}
