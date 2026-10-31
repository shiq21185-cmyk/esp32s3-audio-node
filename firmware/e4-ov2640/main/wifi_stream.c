/* e4 Wi-Fi 推流 —— 照抄乐鑫官方 CameraWebServer 的路子（app_httpd.cpp）。
 *
 * 为什么要它：串口 1.5Mbps 传一帧 10.8KB 要 72ms，帧率被压在 ~10fps。
 * Wi-Fi 下同一帧只要 3~5ms，瓶颈回到传感器本身。官方例程的 stream_handler
 * 就是 "fb_get -> 发 multipart 分片 -> fb_return" 的裸循环，没有任何花样。
 *
 * 端点：
 *   /            一页 HTML（<img src="/stream">）
 *   /stream      MJPEG（multipart/x-mixed-replace）
 *   /snap.jpg    单帧
 *   /stats       帧率/帧大小 JSON
 *   /ctl?...     传感器调参：q=<0..63> vflip hflip bright contrast sat ae gain
 *
 * ⚠️ 这里和串口推帧抢同一套 fb（fb_count=2）：两边同时要图会互相丢帧，
 *    但不会崩。调画质时用 Wi-Fi，别同时开 PUSH 1。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "secrets.h"

static const char *TAG = "e4web";

/* mqtt_cam.c：联网之后再起（见 on_wifi_event） */
void mqtt_cam_start(void);

#define BOUNDARY "frame"
static const char *STREAM_TYPE =
    "multipart/x-mixed-replace;boundary=" BOUNDARY;
static const char *STREAM_PART =
    "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n\r\n";

static httpd_handle_t s_server = NULL;
static char s_ip[16] = "0.0.0.0";
static volatile int s_clients = 0;
static volatile uint32_t s_last_len = 0;
static volatile int64_t s_last_us = 0;

/* ---------------- 页面 ---------------- */

static const char INDEX_HTML[] =
    "<!doctype html><meta charset='utf-8'><title>OV2640</title>"
    "<style>html,body{margin:0;background:#111;color:#ccc;font:13px system-ui}"
    "img{max-width:100%;max-height:100vh;display:block;margin:auto}</style>"
    "<img src='/stream'><p style='text-align:center'>"
    "<a style='color:#6cf' href='/snap.jpg'>单帧</a> · "
    "<a style='color:#6cf' href='/stats'>stats</a></p>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------------- 单帧 ---------------- */

static esp_err_t snap_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
    return res;
}

/* ---------------- MJPEG 流 ---------------- */

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    s_clients++;
    char part[128];
    int fails = 0;

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            /* 不立刻放弃：传感器偶发取帧失败，退一步再试；连续失败太多才断流 */
            if (++fails > 20) {
                ESP_LOGW(TAG, "连续取帧失败，断开这个客户端");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        fails = 0;
        s_last_len = fb->len;
        s_last_us = esp_timer_get_time();

        size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, part, hlen);
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;   /* 浏览器关了页面 */
    }

    s_clients--;
    s_clients = s_clients < 0 ? 0 : s_clients;
    return res;
}

/* ---------------- 统计 ---------------- */

static esp_err_t stats_handler(httpd_req_t *req)
{
    char buf[192];
    int len = snprintf(buf, sizeof(buf),
                       "{\"clients\":%d,\"bytes\":%u,\"age_ms\":%d}\n",
                       s_clients, (unsigned)s_last_len,
                       s_last_us ? (int)((esp_timer_get_time() - s_last_us) / 1000) : -1);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---------------- 传感器调参 ---------------- */

static int qparam(httpd_req_t *req, const char *key, int def)
{
    char val[16];
    if (httpd_req_get_url_query_len(req) <= 0) return def;
    if (httpd_req_get_url_query_str(req, val, sizeof(val)) != ESP_OK) return def;
    char out[16];
    if (httpd_query_key_value(val, key, out, sizeof(out)) != ESP_OK) return def;
    return atoi(out);
}

static esp_err_t ctl_handler(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int v;
    if ((v = qparam(req, "q", -1)) >= 0) s->set_quality(s, v);
    if ((v = qparam(req, "vflip", -1)) >= 0) s->set_vflip(s, v);
    if ((v = qparam(req, "hflip", -1)) >= 0) s->set_hmirror(s, v);
    if ((v = qparam(req, "bright", -999)) != -999) s->set_brightness(s, v);
    if ((v = qparam(req, "contrast", -999)) != -999) s->set_contrast(s, v);
    if ((v = qparam(req, "sat", -999)) != -999) s->set_saturation(s, v);
    if ((v = qparam(req, "gain", -1)) >= 0) s->set_gainceiling(s, (gainceiling_t)v);
    if ((v = qparam(req, "ae", -999)) != -999) s->set_ae_level(s, v);

    char buf[160];
    int len = snprintf(buf, sizeof(buf),
                       "{\"q\":%d,\"vflip\":%d,\"hflip\":%d,\"bright\":%d,"
                       "\"contrast\":%d,\"sat\":%d}\n",
                       s->status.quality, s->status.vflip, s->status.hmirror,
                       s->status.brightness, s->status.contrast, s->status.saturation);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static void start_server(void)
{
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd 启动失败");
        s_server = NULL;
        return;
    }
    httpd_uri_t uris[] = {
        {.uri = "/",          .method = HTTP_GET, .handler = index_handler},
        {.uri = "/stream",    .method = HTTP_GET, .handler = stream_handler},
        {.uri = "/snap.jpg",  .method = HTTP_GET, .handler = snap_handler},
        {.uri = "/stats",     .method = HTTP_GET, .handler = stats_handler},
        {.uri = "/ctl",       .method = HTTP_GET, .handler = ctl_handler},
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP 服务已起：http://%s/", s_ip);
}

/* ---------------- Wi-Fi ---------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi 断开，2 秒后重连");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "IP: %s", s_ip);
        start_server();
        /* MQTT 必须等联网之后再启动：先起会 "Error transport connect"，
         * 之后要等它自己的重连退避，白慢十几秒。 */
        static bool mqtt_started = false;
        if (!mqtt_started) {
            mqtt_started = true;
            mqtt_cam_start();
        }
    }
}

void wifi_stream_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &on_wifi_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 关省电：MJPEG 要的是稳定低延迟，不是省那一两毫安 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "正在连 Wi-Fi: %s", WIFI_SSID);
}

const char *wifi_stream_ip(void)
{
    return s_ip;
}
