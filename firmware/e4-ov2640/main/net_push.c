/* net_push.c —— 板子主动把 JPEG 帧推给 PC（TCP 客户端）
 *
 * 为什么要有这条路：
 *   板子在 Wi-Fi 网段（192.168.1.x），PC 在以太网网段（192.168.77.x），
 *   两个网段被 NAT 隔开 —— PC 连不进板子（浏览器打不开 192.168.1.105），
 *   但板子能连出去（实测 NET 192.168.77.5:8765 → 通，184ms）。
 *   所以方向反过来：板子当客户端，PC 跑服务器收帧再转成本机网页。
 *
 * 帧格式和串口那条一模一样，PC 端解析代码可以直接复用：
 *     META,JPEG,<字节数>\n
 *     <JPEG 原始字节>
 *     \nEND\n
 *
 * 谁取帧：只有这个任务取（main.c 的循环在推流模式下 sleep 不取），
 * 否则两边抢 fb_count=2 的缓冲，帧率直接对半砍。 */

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "lwip/sockets.h"

#include "secrets.h"

#ifndef PUSH_HOST
#define PUSH_HOST "192.168.77.5"   /* PC 上跑 tools/cam_wifi.py 的那台机器 */
#endif
#ifndef PUSH_PORT
#define PUSH_PORT 8767
#endif

static const char *TAG = "push";

static volatile bool s_have_client = false;

bool net_push_connected(void)
{
    return s_have_client;
}

static int tcp_connect(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PUSH_PORT);
    if (inet_pton(AF_INET, PUSH_HOST, &a.sin_addr) != 1) {
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 非阻塞 connect + select 3 秒超时。
     * 用阻塞 connect 的话，对端不在线时要等内核重传几十秒，日志一片沉默，
     * 看起来像"卡死"；非阻塞版每 3 秒一定给出一句话。 */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (rc < 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    if (rc < 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        rc = select(s + 1, NULL, &wf, NULL, &tv);
        if (rc <= 0) {
            close(s);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(s);
            return -1;
        }
    }
    fcntl(s, F_SETFL, fl);
    return s;
}

static int send_all(int s, const void *p, size_t n)
{
    const uint8_t *q = (const uint8_t *)p;
    while (n) {
        int w = send(s, q, n, 0);
        if (w <= 0) {
            return -1;
        }
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static void push_task(void *arg)
{
    char hdr[48];
    int backoff_ms = 1000;

    for (;;) {
        int s = tcp_connect();
        if (s < 0) {
            ESP_LOGW(TAG, "连不上 %s:%d，%dms 后重试", PUSH_HOST, PUSH_PORT, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            if (backoff_ms < 8000) {
                backoff_ms *= 2;
            }
            continue;
        }

        backoff_ms = 1000;
        s_have_client = true;
        ESP_LOGI(TAG, "已连上 %s:%d，开始推流", PUSH_HOST, PUSH_PORT);

        int nf = 0;
        int64_t t0 = esp_timer_get_time();
        for (;;) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            int hl = snprintf(hdr, sizeof(hdr), "META,JPEG,%u\n", (unsigned)fb->len);
            int rc = send_all(s, hdr, (size_t)hl);
            if (rc == 0) {
                rc = send_all(s, fb->buf, fb->len);
            }
            if (rc == 0) {
                rc = send_all(s, "\nEND\n", 5);
            }
            esp_camera_fb_return(fb);

            if (rc != 0) {
                break;
            }
            nf++;
            int64_t dt = esp_timer_get_time() - t0;
            if (dt > 2000000) {
                ESP_LOGI(TAG, "%.1f fps（%d 帧 / %.1fs）",
                         nf * 1000000.0 / (double)dt, nf, dt / 1e6);
                nf = 0;
                t0 = esp_timer_get_time();
            }
        }

        s_have_client = false;
        close(s);
        ESP_LOGW(TAG, "连接断了，重连");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void net_push_start(void)
{
    xTaskCreate(push_task, "push", 4096, NULL, 4, NULL);
}
