/* mqtt_cam.c —— 走 MQTT 推 JPEG（跨网看图）
 *
 * 为什么要有这条：板子在 Wi-Fi 网段、PC 在以太网网段，中间是 NAT，
 * 直连要么靠反向 socket（实测 0.3~2fps，抖），要么就走 broker。
 * MQTT 两边都只做出站连接，跨网最省事；代价是多一跳云、延迟高一点。
 *
 * 协议（和 e2-camera 那套一致，PC 端 tools/cam_view.py 直接能用）：
 *     板子 → PC   <base>/cam/jpeg   二进制约 10~30KB（就一帧 JPEG）
 *     PC → 板子   <base>/cam/cmd    文本："STREAM 1|0" / "SNAP" / "RATE 200" / "BAR 1|0" / "HELP"
 *
 * ⚠️ 流量：VGA 一帧约 10KB，5fps ≈ 50KB/s ≈ 180MB/小时。
 *    EMQX Serverless 免费额度 1GB/月 —— 所以默认 **不推流**，
 *    要图先发 "STREAM 1"，看完记得 "STREAM 0"。 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_timer.h"

#include "secrets.h"

#ifndef CAM_TOPIC_BASE
#define CAM_TOPIC_BASE "whale-8f2a5c31"
#endif
#ifndef BROKER_URI
#define BROKER_URI "mqtts://i1e78188.ala.cn-hangzhou.emqxsl.cn:8883"
#endif

static const char *TAG = "mqttcam";

/* net_push.c 的：反向 socket 那条连着时，这里别跟着抢帧 */
bool net_push_connected(void);

static esp_mqtt_client_handle_t s_cli;
static volatile bool s_online = false;
static volatile bool s_stream = false;
static volatile int s_rate_ms = 200;
static volatile int s_cnt, s_fail;      /* 5fps：够看，也给云省流量 */
static char s_topic_jpeg[96];
static char s_topic_cmd[96];

bool mqtt_cam_streaming(void)
{
    return s_online && s_stream;
}

static void pub_jpeg(void)
{
    if (!s_online) {
        return;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        return;
    }
    if (fb->format == PIXFORMAT_JPEG) {
        int id = esp_mqtt_client_publish(s_cli, s_topic_jpeg,
                                         (const char *)fb->buf, (int)fb->len, 0, 0);
        if (id < 0) {
            s_fail++;
            ESP_LOGW(TAG, "发布失败（%u 字节），可能超了出缓冲", (unsigned)fb->len);
        } else {
            s_cnt++;
        }
    }
    esp_camera_fb_return(fb);
}

/* ---------------- 远程命令 ---------------- */
static void on_cmd(const char *cmd, int len)
{
    ESP_LOGI(TAG, "命令：%.*s", len, cmd);

    if (!strncmp(cmd, "STREAM", 6)) {
        s_stream = (strstr(cmd, "1") != NULL);
        ESP_LOGI(TAG, "推流 => %s（%d ms/帧）", s_stream ? "开" : "关", s_rate_ms);
    } else if (!strncmp(cmd, "SNAP", 4)) {
        pub_jpeg();
        ESP_LOGI(TAG, "单张已发");
    } else if (!strncmp(cmd, "RATE", 4)) {
        int ms = atoi(cmd + 4);
        if (ms >= 0) {
            s_rate_ms = ms;
            ESP_LOGI(TAG, "帧间隔 %d ms（约 %.1f fps）", ms, ms ? 1000.0 / ms : 0.0);
        }
    } else if (!strncmp(cmd, "BAR", 3)) {
        sensor_t *s = esp_camera_sensor_get();
        bool on = (strstr(cmd, "1") != NULL);
        if (s && s->set_colorbar) {
            s->set_colorbar(s, on);
        }
        ESP_LOGI(TAG, "彩条 => %s", on ? "开" : "关");
    } else if (!strncmp(cmd, "SIZE", 4)) {
        /* 跨云推流时帧越大越吃亏（10KB/帧靠上行带宽，3KB/帧能快三倍）。
         * 只改分辨率，不动格式 —— JPEG 不用重启传感器。 */
        sensor_t *s = esp_camera_sensor_get();
        framesize_t fs = FRAMESIZE_VGA;
        const char *name = "VGA 640x480";
        if (strstr(cmd, "QVGA")) {
            fs = FRAMESIZE_QVGA; name = "QVGA 320x240";
        } else if (strstr(cmd, "SVGA")) {
            fs = FRAMESIZE_SVGA; name = "SVGA 800x600";
        } else if (strstr(cmd, "UXGA")) {
            fs = FRAMESIZE_UXGA; name = "UXGA 1600x1200";
        }
        if (s && s->set_framesize) {
            s->set_framesize(s, fs);
        }
        ESP_LOGI(TAG, "分辨率 => %s", name);
    } else if (!strncmp(cmd, "HELP", 4)) {
        ESP_LOGI(TAG, "STREAM 1|0 | SNAP | RATE ms | SIZE VGA|QVGA|SVGA|UXGA | BAR 1|0 | HELP");
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_online = true;
        esp_mqtt_client_subscribe(s_cli, s_topic_cmd, 0);
        ESP_LOGI(TAG, "已连上 broker，订阅 %s", s_topic_cmd);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_online = false;
        ESP_LOGW(TAG, "broker 断开，自动重连中");
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(s_topic_cmd)) {
            on_cmd(ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 错误");
        break;
    default:
        break;
    }
}

static void stream_task(void *arg)
{
    int64_t last = esp_timer_get_time();
    int last_cnt = 0;
    for (;;) {
        if (mqtt_cam_streaming()) {
            pub_jpeg();
            /* 不推的时候也每 200ms 取一帧丢掉：DMA 不停、AE 继续收敛 */
        } else if (s_online && !net_push_connected()) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                esp_camera_fb_return(fb);
            }
        }
        /* 每 5 秒报一次板子侧的推流速度：和 PC 收到的帧数一比，
         * 就能分清是"板子没发出来"还是"云到 PC 这段丢了"。 */
        int64_t now = esp_timer_get_time();
        if (now - last >= 5000000) {
            if (s_stream) {
                ESP_LOGI(TAG, "板侧推流：%.1f fps（%d 帧/%.0fs，失败 %d）",
                         (s_cnt - last_cnt) * 1e6 / (now - last),
                         s_cnt - last_cnt, (now - last) / 1e6, s_fail);
            }
            last = now;
            last_cnt = s_cnt;
        }
        vTaskDelay(pdMS_TO_TICKS(s_rate_ms > 0 ? s_rate_ms : 100));
    }
}

void mqtt_cam_start(void)
{
    snprintf(s_topic_jpeg, sizeof(s_topic_jpeg), "%s/cam/jpeg", CAM_TOPIC_BASE);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/cam/cmd", CAM_TOPIC_BASE);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BROKER_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .session.keepalive = 30,
        .buffer.size = 8192,
        .buffer.out_size = 65536,   /* 一帧 20KB+，出缓冲要够大 */
        .network.timeout_ms = 10000,
    };
    s_cli = esp_mqtt_client_init(&cfg);
    if (!s_cli) {
        ESP_LOGE(TAG, "mqtt 客户端创建失败");
        return;
    }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_cli);

    xTaskCreate(stream_task, "mqttcam", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "上行 %s（默认不推，发 STREAM 1 开始）", s_topic_jpeg);
}
