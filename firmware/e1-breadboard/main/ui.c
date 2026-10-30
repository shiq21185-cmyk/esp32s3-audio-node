/* 屏幕上的那张脸 / 那个小人 + 状态栏。
 *
 * 这不是"仪表盘"，是**表现层**：屋里的人扫一眼就该知道
 *   - 板子在线吗（顶部状态栏：Wi-Fi / MQTT / 麦克风）
 *   - 她现在在听吗（小人抬手 / 眼睛睁大）
 *   - 她正在说吗（嘴一开一合、手臂轻摆）
 *   - 声音有多大（底部电平条，跟着 rms 走）
 *
 * 为什么用"人"而不是一堆数字：这套身体的最终目标是"把开心演成动作"，
 * 眼睛和嘴是最省像素、也最容易被读懂的两种状态。
 *
 * 两套版式，按屏幕方向自动选（都是编译期定死的，不占运行开销）：
 *   竖装（64x128）  —— 一个瘦瘦的小人，从上到下把整块屏用满
 *   横装（128x64）  —— 一张宽脸 + 底部电平条
 * 判据是 OLED_H > OLED_W，也就是 config.h 里 OLED_ROT 设成了竖的那种。
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled.h"

static const char *TAG = "ui";

#define PORTRAIT OLED_PORTRAIT

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

/* ================= 基础图形 ================= */
/* 实心椭圆（整数判据，不引浮点）。on=false = 挖黑（画眼窝/嘴）。
 * 一开始画的是空心方框，1:1 摆在屏上像两块"眼镜片"，不像脸；
 * 换成实心 + 挖黑瞳孔之后才像一张脸。 */
static void ellipse_fill(int cx, int cy, int rx, int ry, bool on)
{
    for (int dy = -ry; dy <= ry; dy++) {
        for (int dx = -rx; dx <= rx; dx++) {
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) {
                oled_pixel(cx + dx, cy + dy, on);
            }
        }
    }
}

static void fill_ellipse(int cx, int cy, int rx, int ry)
{
    ellipse_fill(cx, cy, rx, ry, true);
}

/* 粗线段（画手臂/腿用）。Bresenham 太细，直接沿线扫方块。 */
static void thick_line(int x0, int y0, int x1, int y1, int w)
{
    int n = abs(x1 - x0);
    if (abs(y1 - y0) > n) n = abs(y1 - y0);
    if (n < 1) n = 1;
    int r = w / 2;
    for (int i = 0; i <= n; i++) {
        int x = x0 + (x1 - x0) * i / n;
        int y = y0 + (y1 - y0) * i / n;
        oled_fill(x - r, y - r, w, w, true);
    }
}

/* ================= 顶部状态栏 ================= */
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

static void draw_status(const ui_status_t *st, ui_state_t s)
{
    oled_fill(0, 0, OLED_W, 9, true);          /* 实心白条，字反色 */

    /* 左：模式（G=室内门限 / B=随身按键） */
    oled_text(1, 1, st->mode ? "B" : "G", 1, true);

    /* Wi-Fi 信号柱 */
    draw_wifi(9, 1, st->wifi_ok);

    /* MQTT：空心方块 = 已连；中间挖个黑点 = 没连 */
    oled_fill(22, 2, 6, 6, false);
    if (!st->mqtt_ok) oled_fill(24, 4, 2, 2, true);

    /* 麦克风开着就在状态栏里点一下，免得只看脸误判 */
    if (st->mic_on) {
        oled_fill(31, 1, 3, 5, false);
        oled_vline(30, 4, 3);
        oled_vline(34, 5, 2);
    }

#if PORTRAIT
    /* 竖屏只有 64 px 宽，塞不下 IP —— 右边给一个状态字母，
     * 掉线和待机在黑白屏上都是"静止的小人"，有个字母就不用猜。 */
    static const char code[] = { 'O', 'I', 'L', 'S' };
    char one[2] = { code[s], 0 };
    oled_text(OLED_W - 7, 1, one, 1, true);
#else
    /* 右：IP（连上才有；没连上写 no net） */
    const char *right = st->wifi_ok ? (st->ip[0] ? st->ip : "no ip") : "no net";
    int w = oled_text_w(right, 1);
    oled_text(OLED_W - 2 - w, 1, right, 1, true);
#endif
}

/* ================= 脸（横屏版） ================= */
#if !PORTRAIT
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

static void draw_mouth_landscape(ui_state_t s, int64_t t_ms)
{
    const int cx = OLED_W / 2, y = 20;

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

static void draw_face_landscape(const ui_status_t *st, ui_state_t s, bool blink, int64_t t_ms)
{
    int rx = 4, ry = 5, cy = 16;      /* 9x11 的眼睛，中心落在 y=16 */
    bool up = false;

    if (s == ST_LISTEN) { ry = 6; up = true; }   /* 睁大一点 + 瞳孔上移 = 在听 */
    if (s == ST_OFFLINE) { ry = 1; cy = 18; }    /* 压成一条缝 = 没连线 */

    draw_eye(40, cy, rx, ry, blink, up);
    draw_eye(OLED_W - 41, cy, rx, ry, blink, up);
    draw_mouth_landscape(s, t_ms);
}

#endif /* !PORTRAIT */

/* ================= 瘦小人（竖屏版） ================= */
#if PORTRAIT
/* 64 宽 128 高：横着放不下什么，竖着正好塞一个细长的小人。
 *
 * 排版是在 PC 上按 1:1 像素反复试出来的（.tmp/figure_lab.py），有两条硬规矩：
 *   1) 部件之间必须留 1px 缝，否则实心块会糊成一坨黑；
 *   2) 手臂要从躯干外侧起笔，贴着躯干边线画会让轮廓变成 3px 粗的边。
 * 64x128 上能看清的最小"脸"是：实心圆脑袋 + 两个挖黑的方形眼窝 + 一条嘴。
 * 更小（r=10 以下）试过，眼睛和脑袋的留白不够，看着像两个洞。
 *
 * 纵向预算：状态栏 0..8 / 头 16..40 / 脖子 40..42 / 躯干 42..74
 *           / 腿 75..95 / 地面 96 / 电平条 110..117
 */
static void draw_figure(const ui_status_t *st, ui_state_t s, bool blink, int64_t t_ms)
{
    const int cx = OLED_W / 2;        /* 32 */
    const int head_y = 28, head_r = 12;
    const int body_top = 42, body_bot = 74;

    /* --- 头：实心圆，眼睛靠挖黑才有对比（纯描边看不见） --- */
    fill_ellipse(cx, head_y, head_r, head_r);
    /* 呆毛：头顶偏右的一小撮。不画的话正圆脑袋像颗棋子。 */
    thick_line(cx + 4, head_y - head_r + 2, cx + 8, head_y - head_r - 2, 2);

    if (s == ST_OFFLINE) {
        /* 掉线：眼睛压成两条短横线，嘴也拉平 —— 一眼看出"没醒" */
        oled_fill(cx - 7, head_y - 1, 5, 2, false);
        oled_fill(cx + 2, head_y - 1, 5, 2, false);
        oled_fill(cx - 3, head_y + 5, 7, 1, false);
        (void)blink;
    } else {
        int ey = head_y - 1;
        int d = (s == ST_LISTEN) ? 5 : 4;      /* 睁大 = 在听 */
        for (int k = -1; k <= 1; k += 2) {
            int ex = cx + k * 5;
            if (blink) {
                oled_fill(ex - 2, ey, 5, 2, false);
                continue;
            }
            oled_fill(ex - d / 2, ey - d / 2, d, d, false);
            oled_pixel(ex - d / 2, ey - d / 2, true);   /* 左上高光 */
        }

        /* 嘴 */
        int my = head_y + 6;
        if (s == ST_SPEAK) {
            double ph = (double)(t_ms % 200) / 200.0 * 6.2831853;
            int h = 1 + (int)(2.5 * fabs(sin(ph)));
            oled_fill(cx - 2, my - h / 2, 5, h, false);
        } else if (s == ST_LISTEN) {
            oled_fill(cx - 1, my - 1, 3, 3, false);
        } else {
            oled_fill(cx - 2, my, 5, 1, false);
        }
    }

    /* --- 脖子：比躯干窄，和头之间不贴死 --- */
    oled_fill(cx - 2, head_y + head_r - 1, 5, body_top - (head_y + head_r) + 1, true);

    /* --- 躯干：上窄下宽的实心梯形（13 → 17 px 宽） --- */
    for (int y = body_top; y <= body_bot; y++) {
        int k = (y - body_top) * 100 / (body_bot - body_top);   /* 0..100 */
        int half = 6 + k * 2 / 100;
        oled_fill(cx - half, y, half * 2 + 1, 1, true);
    }

    /* --- 手臂：待机垂着，在听抬起来，说话跟着摆 ---
     * x 从 ±10 起笔（躯干在肩膀处半宽 7），留 2px 缝。 */
    int sy = body_top + 3;
    if (s == ST_LISTEN) {
        thick_line(cx - 10, sy, cx - 14, sy - 9, 2);
        thick_line(cx + 10, sy, cx + 14, sy - 9, 2);
    } else if (s == ST_SPEAK) {
        int sw = (int)(2.0 * sin((double)(t_ms % 420) / 420.0 * 6.2831853));
        thick_line(cx - 10, sy, cx - 14, body_bot - 6 + sw, 2);
        thick_line(cx + 10, sy, cx + 14, body_bot - 6 - sw, 2);
    } else {
        thick_line(cx - 10, sy, cx - 14, body_bot - 6, 2);
        thick_line(cx + 10, sy, cx + 14, body_bot - 6, 2);
    }

    /* --- 腿：比躯干窄，中间留缝 --- */
    thick_line(cx - 3, body_bot + 1, cx - 5, body_bot + 18, 3);
    thick_line(cx + 3, body_bot + 1, cx + 5, body_bot + 18, 3);
    /* --- 脚：向外的两根小横线；地面一条长线，让小人站得住 --- */
    oled_fill(cx - 9, body_bot + 19, 6, 2, true);
    oled_fill(cx + 4, body_bot + 19, 6, 2, true);
    oled_hline(cx - 12, body_bot + 22, 25);

    (void)st;
}

#endif /* PORTRAIT */

/* ================= 电平条 ================= */
static void draw_meter(const ui_status_t *st)
{
#if PORTRAIT
    const int y = OLED_H - 16, h = 7;
    const int x = 6, w = OLED_W - 12;
    oled_frame(x, y, w, h);
    int pct = level_pct(st->rms, st->floor);
    int inner = w - 4;
    int fill_w = inner * pct / 100;
    if (fill_w < 1) fill_w = 1;
    oled_fill(x + 2, y + 2, fill_w, h - 4, true);
#else
    const int y = 25, h = 7;
    oled_frame(0, y, OLED_W, h);
    int pct = level_pct(st->rms, st->floor);
    if (pct > 0) {
        int inner = OLED_W - 4;
        int w = inner * pct / 100;
        if (w < 1) w = 1;
        oled_fill(2, y + 2, w, h - 4, true);
    }
#endif
}

/* ================= 只有大屏才画得下的数字区 ================= */
static void draw_extra(const ui_status_t *st, ui_state_t s)
{
#if PORTRAIT
    /* 竖屏没有横向空间摆 "UP 12 DN 34" 这种长串，所以只在
     * 电平条上面留一行极简状态：有按键窗口就写 BTN。 */
    (void)st; (void)s;
#else
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
#endif
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
        draw_status(&st, s);
#if PORTRAIT
        draw_figure(&st, s, blink, t_ms);
#else
        draw_face_landscape(&st, s, blink, t_ms);
#endif
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
    ESP_LOGI(TAG, "UI 起来了（10 fps，%s）", PORTRAIT ? "竖屏小人" : "横屏脸");
}
