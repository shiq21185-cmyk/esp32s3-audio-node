/* 屏幕上的那张脸 + 状态栏。
 *
 * 这不是"仪表盘"，是**表现层**：屋里的人扫一眼就该知道
 *   - 板子在线吗（顶部状态栏：Wi-Fi / MQTT / IP）
 *   - 她现在在听吗（眼睛睁大 + 嘴变成小圈）
 *   - 她正在说吗（嘴一开一合）
 *   - 声音有多大（底部电平条，跟着 rms 走）
 *
 * 为什么用"脸"而不是一堆数字：这套身体的最终目标是"把开心演成动作"，
 * 眼睛和嘴是最省像素、也最容易被读懂的两种状态。数字放在 64 行屏的下半屏。
 *
 * ⚠️ 布局原则：**前 32 行永远是一份完整的 UI**。
 *    0.91" 是 128x32、0.96" 是 128x64，这两种屏在 I2C 上长得一模一样、
 *    没法自动分辨（SSD1306 不能回读尺寸）。所以把主 UI 压在前 32 行：
 *    参数写成 32 而屏是 64 —— 上半屏照样对；写成 64 而屏是 32 ——
 *    上半屏也对，只是多出来的那段看不见。猜错不会变成一堆乱码。
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled.h"

static const char *TAG = "ui";

static ui_status_t (*s_snapshot)(void);
static bool s_started;

/* 眨眼/说话动画都靠这两个量：一个是"下次眨眼时间"，一个是"眨眼结束时间" */
static int64_t s_blink_at_us;
static int64_t s_blink_end_us;
static uint32_t s_rng = 0x1234567u;

static uint32_t rng_next(void)
{
    /* 只用来看动画，不要求质量；省一个 rand() 的依赖 */
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16) & 0x7fffu;
}

static int64_t now_us(void) { return esp_timer_get_time(); }

typedef enum {
    ST_OFFLINE = 0,   /* 没连上 broker（Wi-Fi 可能通也可能不通） */
    ST_IDLE,          /* 在线，但麦克风关着 / 没人在说话 */
    ST_LISTEN,        /* 在听：门限开着或按键窗口内 */
    ST_SPEAK,         /* 她在说话 */
} ui_state_t;

static ui_state_t pick_state(const ui_status_t *st)
{
    if (!st->mqtt_ok) return ST_OFFLINE;
    if (st->speaking) return ST_SPEAK;
    if (st->mic_on || st->gate_open || st->btn_window) return ST_LISTEN;
    return ST_IDLE;
}

/* 音量 → 0..100。用 log2 是因为人耳对响度是指数的：
 * 线性映射会让"安静房间"和"正常说话"挤在最左边 5% 里，条子看着像坏了。 */
static int level_pct(int rms, int floor)
{
    int thr = floor * 2;
    if (thr < 40) thr = 40;
    if (rms <= thr) return 6;              /* 静音也留一格，表示"在听" */
    int v = 0, r = rms;
    while (r > 0) { v++; r >>= 1; }
    int pct = v * 100 / 12;
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    return pct;
}

/* ---------------- 顶部状态栏 ---------------- */
static void draw_wifi(int x, int y, bool on)
{
    /* 4 根柱子，从 2px 高长到 8px 高；没连上就打一个叉 */
    if (!on) {
        /* 状态栏是白底，所以"画一个叉"= 把像素清成黑 */
        for (int i = 0; i < 7; i++) {
            oled_pixel(x + i, y + 1 + i, false);
            oled_pixel(x + 6 - i, y + 1 + i, false);
        }
        return;
    }
    static const int h[4] = { 2, 4, 6, 8 };
    for (int i = 0; i < 4; i++) {
        oled_fill(x + i * 3, y + 8 - h[i], 2, h[i], false);
    }
}

static void draw_status(const ui_status_t *st)
{
    oled_fill(0, 0, OLED_W, 9, true);          /* 实心白条，字反色 */

    /* 左：模式（G=室内门限 / B=随身按键） */
    oled_text(2, 1, st->mode ? "B" : "G", 1, true);

    /* Wi-Fi 信号柱 */
    draw_wifi(11, 1, st->wifi_ok);

    /* MQTT：实心方块 = 已连；空心 = 没连 */
    if (st->mqtt_ok) {
        oled_fill(24, 2, 6, 6, false);
    } else {
        oled_fill(24, 2, 6, 6, false);
        oled_fill(26, 4, 2, 2, true);
    }

    /* 麦克风开着就在状态栏里点一下，免得只看眼睛误判 */
    if (st->mic_on) {
        oled_fill(34, 1, 3, 5, false);
        oled_vline(33, 4, 3);
        oled_vline(37, 4, 3);
    }

    /* 右：IP（连上才有；没连上写 no net） */
    const char *right = st->wifi_ok ? (st->ip[0] ? st->ip : "no ip") : "no net";
    int w = oled_text_w(right, 1);
    oled_text(OLED_W - 2 - w, 1, right, 1, true);
}

/* ---------------- 脸 ---------------- */
/* 实心椭圆（整数判据 dx²ry² + dy²rx² ≤ rx²ry²，不引浮点）。
 * 一开始画的是空心方框，1:1 摆在 128x32 上像两块"眼镜片"，不像眼睛；
 * 换成实心椭圆 + 黑瞳孔之后才像一张脸。 */
static void fill_ellipse(int cx, int cy, int rx, int ry)
{
    for (int dy = -ry; dy <= ry; dy++) {
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) {
                oled_pixel(cx + dx, cy + dy, true);
            }
        }
    }
}

static void draw_eye(int cx, int cy, int rx, int ry, bool blink, bool up)
{
    if (blink || ry <= 1) {
        oled_fill(cx - rx, cy, rx * 2 + 1, 2, true);
        return;
    }
    fill_ellipse(cx, cy, rx, ry);
    /* 瞳孔：靠上 = 在看你（在听）；居中 = 平常。黑瞳孔 = 白眼球里挖个洞。 */
    int py = up ? cy - 1 : cy;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx * dx * 4 + dy * dy <= 4) {
                oled_pixel(cx + dx, py + dy, false);
            }
        }
    }
}

static void draw_mouth(ui_state_t s, int64_t t_ms)
{
    const int cx = 64, y = 20;

    if (s == ST_SPEAK) {
        /* 一开一合：2px 到 7px，周期约 180ms —— 比真实口型快，
         * 但小屏上"动起来"就够了，看的人是靠节奏认出来的 */
        double ph = (double)(t_ms % 180) / 180.0 * 6.2831853;
        int h = 2 + (int)(5.0 * fabs(sin(ph)));
        oled_fill(cx - 5, y - h / 2, 10, h, true);
        return;
    }
    if (s == ST_LISTEN) {
        oled_frame(cx - 4, y - 3, 9, 6);       /* 小圈 = "我在听，你说" */
        return;
    }
    oled_hline(cx - 6, y, 13);                 /* 平嘴 = 待机 */
}

static void draw_face(const ui_status_t *st, ui_state_t s, bool blink, int64_t t_ms)
{
    int rx = 4, ry = 5, cy = 16;      /* 9x11 的眼睛，中心落在 y=16 */
    bool up = false;

    if (s == ST_LISTEN) { ry = 6; up = true; }   /* 睁大一点 + 瞳孔上移 = 在听 */
    if (s == ST_OFFLINE) { ry = 1; cy = 18; }    /* 压成一条缝 = 没连线 */

    draw_eye(40, cy, rx, ry, blink, up);
    draw_eye(87, cy, rx, ry, blink, up);
    draw_mouth(s, t_ms);
}

/* ---------------- 电平条 ---------------- */
static void draw_meter(const ui_status_t *st)
{
    const int y = 25, h = 7;
    oled_frame(0, y, OLED_W, h);
    int pct = level_pct(st->rms, st->floor);
    if (pct > 0) {
        int inner = OLED_W - 4;
        int w = inner * pct / 100;
        if (w < 1) w = 1;
        oled_fill(2, y + 2, w, h - 4, true);
    }
}

/* ---------------- 下半屏（只有 128x64 才有地方） ---------------- */
static void draw_extra(const ui_status_t *st, ui_state_t s)
{
    char buf[24];
    if (oled_height() < 64) return;

    snprintf(buf, sizeof(buf), "UP %lu DN %lu",
             (unsigned long)st->up_pkts, (unsigned long)st->down_pkts);
    oled_text(0, 34, buf, 1, false);

    snprintf(buf, sizeof(buf), "IP %s", st->ip[0] ? st->ip : "-");
    oled_text(0, 42, buf, 1, false);

    snprintf(buf, sizeof(buf), "MIC %s GATE %s",
             st->mic_on ? "ON" : "OFF", st->gate_open ? "OPEN" : "shut");
    oled_text(0, 50, buf, 1, false);

    static const char *names[] = { "OFFLINE", "IDLE", "LISTEN", "SPEAK" };
    snprintf(buf, sizeof(buf), "%s%s", names[s], st->btn_window ? " BTN" : "");
    oled_text(0, 58, buf, 1, false);
}

static void ui_task(void *arg)
{
    (void)arg;
    int64_t t0 = now_us();
    s_blink_at_us = t0 + 2500000;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_snapshot) continue;

        ui_status_t st = s_snapshot();
        int64_t t = now_us();
        int64_t t_ms = (t - t0) / 1000;

        /* 眨眼：3~6 秒一次，闭 120ms。随机的目的是别让两块板/两轮演示同步。 */
        bool blink = false;
        if (t >= s_blink_at_us && t >= s_blink_end_us) {
            s_blink_end_us = t + 120000;
            s_blink_at_us = s_blink_end_us + 3000000 + (int64_t)(rng_next() % 3000000);
        }
        if (t < s_blink_end_us) blink = true;

        ui_state_t s = pick_state(&st);

        oled_clear();
        draw_status(&st);
        draw_face(&st, s, blink, t_ms);
        draw_meter(&st);
        draw_extra(&st, s);
        oled_flush();
    }
}

void ui_start(ui_status_t (*snapshot)(void))
{
    if (s_started) return;
    if (!oled_ready()) {
        ESP_LOGW(TAG, "没有屏幕，UI 不起");
        return;
    }
    s_snapshot = snapshot;
    s_started = true;
    xTaskCreate(ui_task, "ui", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "UI 起来了（10 fps）");
}
