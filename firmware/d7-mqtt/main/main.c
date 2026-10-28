/*
 * d7-mqtt —— 让板子和电脑「都不在同一个局域网也能说话」
 *
 * 前面 D3/D4 都是用 UDP 点对点：电脑直接往板子 IP 发包。
 * 这在同一张网里没问题，但身体一旦被带出实验室：
 *   - 板子连的是 4G / 别人的 Wi-Fi，躲在运营商或路由器的 NAT 后面
 *   - 电脑根本找不到它
 *
 * 解法：两边都不主动"连对方"，而是各自往外连一个公共中转（MQTT broker）。
 *   PC  --出站-->  broker  <--出站--  板子
 * 谁的 IP 变了都无所谓，只要能上外网就行。
 *
 * 这一步（D7）只验证"管道通不通"：
 *   - 板子连上 broker
 *   - 每 5 秒发一条自己的状态（signal / heap / 在线时长）
 *   - 电脑发一条指令，板子能收到并打出来
 * 音频还在后面（要压缩，见 D8）。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "mqtt_client.h"

#include "secrets.h"

static const char *TAG = "d7";

/* ---------- 中转服务器 ---------- */
/* 先用公共 broker 做技术验证；正式跑要换成自己的（隐私 + 稳定）。
 * 换地址只改这一行，别的地方不用动。 */
#define BROKER_URI        "mqtt://broker.emqx.io:1883"
#define STATE_PERIOD_MS   5000
#define MQTT_KEEPALIVE_S  30

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
        s_retry++;
        ESP_LOGW(TAG, "Wi-Fi 断开 (reason=%d)，第 %d 次重连…", d->reason, s_retry);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 通了，本机 IP = " IPSTR, IP2STR(&e->ip_info.ip));
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

    EventBits_t bits = xEventGroupWaitBits(s_net_ev, BIT_CONNECTED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "没连上 Wi-Fi。查：名字/密码、是不是 5GHz");
        return ESP_FAIL;
    }

    /* 低延迟，关省电（否则 ping 会从个位数掉到 70ms 上下） */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* ---------- MQTT ---------- */
static char s_base[40];                     /* 主题前缀：whale-xxxxxx */
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_connected;
static int s_rx_count;

static void publish_state(const char *kind)
{
    char topic[80], payload[220];

    wifi_ap_record_t ap = { 0 };
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip);
    }

    snprintf(topic, sizeof(topic), "%s/state", s_base);
    snprintf(payload, sizeof(payload),
             "{\"kind\":\"%s\",\"up\":%lu,\"rssi\":%d,\"heap\":%lu,"
             "\"rx\":%d,\"ip\":\"" IPSTR "\"}",
             kind, (unsigned long)(esp_timer_get_time() / 1000000), rssi,
             (unsigned long)esp_get_free_heap_size(), s_rx_count, IP2STR(&ip.ip));

    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 0, 0);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        char topic[80];
        snprintf(topic, sizeof(topic), "%s/cmd", s_base);
        esp_mqtt_client_subscribe(s_mqtt, topic, 0);
        ESP_LOGI(TAG, "==================================");
        ESP_LOGI(TAG, "  连上 broker 了！主题前缀 = %s", s_base);
        ESP_LOGI(TAG, "  订阅: %s", topic);
        ESP_LOGI(TAG, "==================================");
        publish_state("hello");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "和 broker 断开，会自动重连");
        break;
    case MQTT_EVENT_DATA: {
        s_rx_count++;
        char topic[100] = { 0 };
        char payload[512] = { 0 };
        int tl = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
        int pl = ev->data_len  < (int)sizeof(payload) - 1 ? ev->data_len  : (int)sizeof(payload) - 1;
        if (ev->topic) memcpy(topic, ev->topic, tl);
        if (ev->data)  memcpy(payload, ev->data, pl);
        ESP_LOGI(TAG, "收到 [%s] %s", topic, payload);
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 出错（网络抖动会触发，通常自动恢复）");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 主题前缀用 MAC 后三位，保证两台板子不打架 */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_base, sizeof(s_base), "whale-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "我的 MAC = %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "主题前缀 = %s", s_base);

    if (wifi_connect() != ESP_OK) {
        return;
    }

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.uri = BROKER_URI;
    cfg.session.keepalive = MQTT_KEEPALIVE_S;
    cfg.network.reconnect_timeout_ms = 3000;
    cfg.credentials.client_id = s_base;

    ESP_LOGI(TAG, "正在连中转 %s …", BROKER_URI);
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATE_PERIOD_MS));
        if (s_connected) {
            publish_state("tick");
        }
    }
}
