/**
 * audio-node --- a tiny Wi-Fi audio endpoint on ESP32-S3.
 *
 * What it does, in one sentence: it is a microphone and a speaker that
 * happen to live on the network.
 *
 *   mic  -> I2S RX -> UDP packets         (upstream, ~20 ms per packet)
 *   UDP packets -> I2S TX -> speaker      (downstream)
 *
 * No codec, no framing layer, no cloud. Raw 16 kHz / 16 bit / stereo
 * PCM in both directions, plus four text commands (PING / HELLO / STOP).
 * See docs/protocol.md for the wire format.
 *
 * Design notes worth knowing before you edit:
 *
 * 1. The speaker has its own task that *never stops writing* to I2S.
 *    When there is nothing to play it writes zeros. This is not
 *    belt-and-braces: the ESP32 I2S TX peripheral is a ring of DMA
 *    descriptors and will happily replay the last one forever if you
 *    simply stop feeding it. "Stop playing" must mean "feed silence",
 *    never "stop writing".
 *
 * 2. Wi-Fi power save is turned off. With the default power save the
 *    round trip time wandered around 70 ms; with it off it is a single
 *    digit. Audio latency is worth far more than the few mA.
 *
 * 3. The board learns the host address from the first HELLO instead of
 *    being configured with it, so a DHCP lease change on the PC side
 *    does not need a reflash.
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

#include "config.h"
#include "secrets.h"

static const char *TAG = "audionode";

/* ---------------- Wi-Fi ---------------- */
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
            ESP_LOGW(TAG, "disconnected (reason=%d), retry %d/10", d->reason, s_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up after 10 connection attempts");
            xEventGroupSetBits(s_net_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "==================================");
        ESP_LOGI(TAG, "  connected, board IP = " IPSTR, IP2STR(&e->ip_info.ip));
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
    ESP_LOGI(TAG, "connecting to \"%s\" ...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_net_ev, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "no connection. check: SSID/password, 2.4 GHz only, WPA2 vs WPA3");
        return ESP_FAIL;
    }

    /* Low latency matters more than power here. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* ---------------- I2S ---------------- */
static i2s_chan_handle_t s_rx;  /* microphone */
static i2s_chan_handle_t s_tx;  /* amplifier */
static StreamBufferHandle_t s_spk;

static esp_err_t audio_init(void)
{
    /* Microphone: two I2S controllers, because I2S0 and I2S1 are separate
     * peripherals and we do not share a bit clock between mic and amp. */
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

    /* Amplifier. auto_clear makes the DMA ring read as zeros when it runs
     * dry; combined with the always-writing task it removes the "stuck
     * repeating the last buffer" failure mode entirely. */
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

    s_spk = xStreamBufferCreate(SPK_BUFFER_BYTES, CHUNK_BYTES / 4);
    return ESP_OK;
}

/* ---------------- speaker task: feed forever ---------------- */
static void spk_task(void *arg)
{
    static int16_t buf[CHUNK_FRAMES * 2];

    while (1) {
        size_t total = xStreamBufferReceive(s_spk, buf, sizeof(buf), pdMS_TO_TICKS(20));

        if (total > 0) {
            /* Drain whatever else is queued so we hand I2S one full 20 ms
             * block; partial blocks would drift the frame boundary. */
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
            /* Nothing queued: write silence, do NOT return. */
            memset(buf, 0, sizeof(buf));
        }

        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

/* ---------------- UDP task ---------------- */
static void udp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket() failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in me = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(UDP_PORT),
    };
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed", UDP_PORT);
        close(sock); vTaskDelete(NULL); return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = SOCK_RX_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "listening on UDP %d (0x%02X-prefixed = audio, else command)",
             UDP_PORT, AUDIO_MAGIC);

    static uint8_t chunk[CHUNK_BYTES];
    static uint8_t rxpkt[2048];
    char cmd[32];

    struct sockaddr_in peer = { 0 };
    bool have_peer = false, streaming = false;
    uint32_t sent = 0, played = 0, dropped = 0;
    TickType_t last_report = xTaskGetTickCount();
#if STREAM_IDLE_TIMEOUT_MS > 0
    TickType_t last_rx = last_report;
#endif

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, rxpkt, sizeof(rxpkt), 0, (struct sockaddr *)&from, &fromlen);

        if (n > 0) {
#if STREAM_IDLE_TIMEOUT_MS > 0
            last_rx = xTaskGetTickCount();
#endif

            if (rxpkt[0] == AUDIO_MAGIC && n > 1) {
                /* Downstream audio: queue it for the speaker task. */
                size_t len = (size_t)(n - 1) & ~3u;  /* whole frames only */
                if (len > 0) {
                    if (xStreamBufferSend(s_spk, rxpkt + 1, len, 0) != len) dropped++;
                    played++;
                }
                continue;
            }

            size_t clen = (size_t)n < sizeof(cmd) - 1 ? (size_t)n : sizeof(cmd) - 1;
            memcpy(cmd, rxpkt, clen);
            cmd[clen] = '\0';
            for (size_t i = 0; i < clen; i++) {
                if (cmd[i] == '\r' || cmd[i] == '\n') { cmd[i] = '\0'; break; }
            }
            ESP_LOGI(TAG, "command \"%s\" from %s:%d", cmd,
                     inet_ntoa(from.sin_addr), ntohs(from.sin_port));

            if (strcmp(cmd, "HELLO") == 0) {
                /* Latest HELLO wins: the host is whoever asked last. */
                peer = from; have_peer = true; streaming = true;
                sent = played = dropped = 0;
                const char *ack = "READY";
                sendto(sock, ack, strlen(ack), 0, (struct sockaddr *)&peer, sizeof(peer));
                ESP_LOGI(TAG, "streaming upstream -> %s:%d",
                         inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            } else if (strcmp(cmd, "STOP") == 0) {
                streaming = false;
                ESP_LOGI(TAG, "stream stopped (sent %lu, played %lu, dropped %lu)",
                         (unsigned long)sent, (unsigned long)played, (unsigned long)dropped);
            } else if (strcmp(cmd, "PING") == 0) {
                const char *pong = "PONG";
                sendto(sock, pong, strlen(pong), 0, (struct sockaddr *)&from, sizeof(from));
            }
            continue;
        }

        if (!streaming || !have_peer) continue;

#if STREAM_IDLE_TIMEOUT_MS > 0
        if ((xTaskGetTickCount() - last_rx) > pdMS_TO_TICKS(STREAM_IDLE_TIMEOUT_MS)) {
            streaming = false;
            ESP_LOGW(TAG, "no packets for %d ms, stopping stream", STREAM_IDLE_TIMEOUT_MS);
            continue;
        }
#endif

        size_t got = 0;
        if (i2s_channel_read(s_rx, chunk, CHUNK_BYTES, &got, 100) != ESP_OK || got == 0) continue;
        sendto(sock, chunk, got, 0, (struct sockaddr *)&peer, sizeof(peer));
        sent++;

        if ((xTaskGetTickCount() - last_report) > pdMS_TO_TICKS(3000)) {
            ESP_LOGI(TAG, "up %lu pkts | played %lu | dropped %lu",
                     (unsigned long)sent, (unsigned long)played, (unsigned long)dropped);
            last_report = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "\n===== esp32s3-audio-node =====");

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
    ESP_LOGI(TAG, "mic BCLK/WS/DIN = %d/%d/%d, amp BCLK/LRC/DOUT = %d/%d/%d, %d Hz",
             PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_DIN,
             PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DOUT, SAMPLE_RATE);

    xTaskCreate(spk_task, "spk", 4096, NULL, 6, NULL);
    xTaskCreate(udp_task, "udp", 8192, NULL, 5, NULL);
}
