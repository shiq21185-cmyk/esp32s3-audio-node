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
#include "sprite.h"

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

#if !PORTRAIT
/* 眨眼/说话动画都靠这两个量：一个是"下次眨眼时间"，一个是"眨眼结束时间" */
static int64_t s_blink_at_us;
static int64_t s_blink_end_us;
#endif
#if !PORTRAIT
static uint32_t s_rng = 0x1234567u;
#endif

#if !PORTRAIT
static uint32_t rng_next(void)
{
    /* 只用来看动画，不要求质量；省一个 rand() 的依赖 */
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16) & 0x7fffu;
}
#endif

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
#if !PORTRAIT
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
#endif

/* ================= 顶部状态栏 ================= */
#if !PORTRAIT
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

#endif /* !PORTRAIT */

#if !PORTRAIT
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

    /* 右：IP（连上才有；没连上写 no net） */
    const char *right = st->wifi_ok ? (st->ip[0] ? st->ip : "no ip") : "no net";
    int w = oled_text_w(right, 1);
    oled_text(OLED_W - 2 - w, 1, right, 1, true);
}
#endif /* !PORTRAIT */

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
/* 64 宽 128 高：脸和身体是**一张点阵图**（sprite.h），不再用几何图元拼。
 *
 * 为什么换掉手画：64x128/1bpp 上"头发、脸、衣服"三者同色，靠黑缝区分时
 * 每一种缝都会读成别的东西（兜帽/头盔/边框/门框）。试了二十多版之后认输，
 * 改成把 GPT 出的线稿直接描成点阵（.tmp/trace4.py：alpha 取身形 + 亮度取黑线），
 * 白身体挖黑线，一眼就能读出双马尾、鳍耳、眼睛、裙子和鲸尾。
 *
 * 动的东西只有一处：**说话时嘴的开合**（主人 m02323 定的：不做别的表情）。
 */
static void draw_sprite(void)
{
    for (int y = 0; y < OLED_H; y++) {
        const uint8_t *row = k_body[y];
        for (int b = 0; b < OLED_W / 8; b++) {
            uint8_t v = row[b];
            if (!v) continue;
            for (int i = 0; i < 8; i++) {
                if (v & (1u << i)) oled_pixel(b * 8 + i, y, true);
            }
        }
    }
}

/* 嘴的窗口：底图上嘴在 (31..32, 45..46)，左右不能碰到眼睛（左眼到 x=28、
 * 右眼从 x=34 起），上下留到脸颊和下巴之间。 */
#define MOUTH_X0 28
#define MOUTH_X1 34
#define MOUTH_Y0 42
#define MOUTH_Y1 49

static void draw_mouth(ui_state_t s, int64_t t_ms)
{
    if (s != ST_SPEAK) return;         /* 其它状态保持底图原样 */

    /* 先把窗口恢复成底图（否则黑嘴会一层层叠上去越张越大） */
    for (int y = MOUTH_Y0; y <= MOUTH_Y1; y++) {
        for (int x = MOUTH_X0; x <= MOUTH_X1; x++) {
            bool on = (k_body[y][x >> 3] >> (x & 7)) & 1;
            oled_pixel(x, y, on);
        }
    }
    /* 张嘴就是 2x2：底图本身已经是 3 个黑点（(31,45)(31,46)(32,46)），
     * 补上 (32,45) 凑成方块 —— 整个"说话"只多一个像素。
     * 主人试过 3x3/3x4/4x4，实机上都像在喊；64px 宽的脸撑不住更大的嘴。 */
    oled_pixel(31, 45, false);
    oled_pixel(32, 45, false);
    oled_pixel(31, 46, false);
    oled_pixel(32, 46, false);
    (void)t_ms;
}
/* 尾巴：最先那版的晃动（绕 (47,72) 转 ±10°、1.6 秒一个来回），主人要的就是这个手感。
 * 当时的问题是根部会甩离裙子露出黑缝 —— 现在根部的像素已经补进裙子里（见 sprite.h 的 STEM），
 * 所以同一个晃法不会再脱开。
 *
 * 它是"从裙子后面钻出来"的，不是长在身上 —— 所以转轴放在裙子边缘而不是尾尖，
 * 位移才有"从裙子后面甩出来"的感觉，且根部几乎不动，不会飞出去。
 * 之前两版都不对：①绕尾尖转 ⇒ 根部被甩离身体露黑缝；②剪切式 ⇒ 整条尾巴上下平移，
 * 看着像在飘，不像在摆。
 */
#define TAIL_WAG 0      /* 0 = 尾巴不动（主人：先别动，以后再改）；1 = 晃 */

static void draw_tail(int64_t t_ms)
{
#if !TAIL_WAG
    (void)t_ms;
    double ang = 0.0;
#else
    double ph = (double)(t_ms % 1600) / 1600.0 * 6.2831853;
    double ang = 10.0 * sin(ph) * 3.14159265358979 / 180.0;
#endif
    double sa = sin(ang), ca = cos(ang);
    const double px = TAIL_PIVOT_X, py = TAIL_PIVOT_Y;

    for (int y = TAIL_Y0 - 6; y < TAIL_Y0 + TAIL_H + 6; y++) {
        if (y < 0 || y >= OLED_H) continue;
        for (int x = TAIL_X0 - 3; x < TAIL_X0 + TAIL_W + 6; x++) {
            if (x < 0 || x >= OLED_W) continue;
            double dx = (double)x - px, dy = (double)y - py;
            double sx = px + dx * ca + dy * sa;
            double sy = py - dx * sa + dy * ca;
            int tx = (int)(sx + 0.5) - TAIL_X0, ty = (int)(sy + 0.5) - TAIL_Y0;
            if (tx < 0 || tx >= TAIL_W || ty < 0 || ty >= TAIL_H) continue;
            if ((k_tail[ty][tx >> 3] >> (tx & 7)) & 1) oled_pixel(x, y, true);
        }
    }
}
#endif /* PORTRAIT */

/* ================= 电平条 ================= */
static void draw_meter(const ui_status_t *st)
{
#if PORTRAIT
    const int y = OLED_H - 8, h = 7;      /* 点阵底图地面线在 y=117，电平条让到最底 */
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
#if !PORTRAIT
    s_blink_at_us = t0 + 2500000;
#endif

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_snapshot) continue;

        ui_status_t st = s_snapshot();
        int64_t t = now_us();
        int64_t t_ms = (t - t0) / 1000;

#if !PORTRAIT
        /* 眨眼：3~6 秒一次，闭 120ms。随机的目的是别让两块板/两轮演示同步。
         * 竖屏是点阵小人，只动嘴（主人定的），所以这段只在横屏编。 */
        bool blink = false;
        if (t >= s_blink_at_us && t >= s_blink_end_us) {
            s_blink_end_us = t + 120000;
            s_blink_at_us = s_blink_end_us + 3000000 + (int64_t)(rng_next() % 3000000);
        }
        if (t < s_blink_end_us) blink = true;
#endif

        ui_state_t s = pick_state(&st);

        oled_clear();
#if !PORTRAIT
        draw_status(&st, s);
#endif
#if PORTRAIT
        draw_sprite();
        draw_tail(t_ms);
        draw_mouth(s, t_ms);
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
