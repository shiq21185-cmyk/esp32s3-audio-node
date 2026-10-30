/* 0.91"/0.96" I2C OLED（SSD1306）—— 一个只做"够用"的驱动。
 *
 * 为什么不用 LVGL / 不抄小智那套：
 *   小智的屏幕是 LVGL 画的（OledDisplay : LvglDisplay），一进来就是几百 KB 的
 *   图形库 + 字体资源。我们这块板只显示几十个字符和两个眼睛，
 *   为它拖一套 GUI 框架不值得 —— 而且我们的固件是纯 C + 单文件风格。
 *
 * 所以这里就是：I2C 总线 + IDF 自带的 ssd1306 panel 驱动 + 一帧 1bpp 点阵缓冲
 * + 几个画点/画框/写字的函数。整块 128x64 的缓冲只有 1KB。
 *
 * ⚠️ 屏幕是"可选件"：没接、接错、地址不对，oled_init() 只会打一行日志返回 false，
 *    音频和 MQTT 照常跑。别为了屏幕把整机搞得起不来。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OLED_W 128

/* 屏幕尺寸要跟实物对得上（见 config.h 的 OLED_HEIGHT）。
 * 猜错的后果是只显示上半屏或多出一截 —— 驱动本身不会报错。 */
bool oled_init(void);
bool oled_ready(void);
int  oled_height(void);

/* 帧缓冲操作（都不碰硬件，只有 oled_flush() 才真正写 I2C） */
void oled_clear(void);
void oled_flush(void);
void oled_pixel(int x, int y, bool on);
void oled_fill(int x, int y, int w, int h, bool on);
void oled_frame(int x, int y, int w, int h);        /* 1px 空心框 */
void oled_hline(int x, int y, int w);
void oled_vline(int x, int y, int h);

/* 写字：scale=1 时 6x8 一个字符，scale=2 时 12x16。
 * inv=true 表示"反色写"（把像素清掉）—— 用在顶部那条实心状态栏上。 */
void oled_text(int x, int y, const char *s, int scale, bool inv);
int  oled_text_w(const char *s, int scale);
