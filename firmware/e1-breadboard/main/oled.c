#include "oled.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "config.h"
#include "font5x7.h"

#define TAG "oled"

#if !OLED_ENABLE

bool oled_init(void) { return false; }
bool oled_ready(void) { return false; }
int  oled_height(void) { return 0; }
void oled_clear(void) {}
void oled_flush(void) {}
void oled_pixel(int x, int y, bool on) { (void)x; (void)y; (void)on; }
void oled_fill(int x, int y, int w, int h, bool on) { (void)x; (void)y; (void)w; (void)h; (void)on; }
void oled_frame(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void oled_hline(int x, int y, int w) { (void)x; (void)y; (void)w; }
void oled_vline(int x, int y, int h) { (void)x; (void)y; (void)h; }
void oled_text(int x, int y, const char *s, int scale, bool inv)
{
    (void)x; (void)y; (void)s; (void)scale; (void)inv;
}
int oled_text_w(const char *s, int scale) { (void)s; (void)scale; return 0; }

#else  /* OLED_ENABLE */

/* SSD1306 的显存是"页"结构：一页 8 行，每页里每个字节是 8 个纵向像素
 * （bit0 在最上）。这正好就是我们的帧缓冲布局，不需要再做转换：
 *   fb[(y / 8) * 128 + x] 的 bit (y % 8) */
static uint8_t s_fb[OLED_W * OLED_HEIGHT / 8];

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

bool oled_ready(void) { return s_ready; }
int  oled_height(void) { return OLED_HEIGHT; }

bool oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C 总线起不来（%s），屏幕不用了", esp_err_to_name(err));
        return false;
    }

    /* 先探一下 0x3C。没接屏的时候 i2c_new_master_bus 也会成功，
     * 真正的失败要等到第一次写命令 —— 那时候报错信息很含糊（ESP_ERR_TIMEOUT），
     * 所以先探一次，日志里能直接说"没找到屏"。 */
    if (i2c_master_probe(bus, OLED_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "I2C 上没有 0x%02X（SDA=%d SCL=%d）—— 没接屏或接线不对，跳过",
                 OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
        return false;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,          /* SSD1306 的 Co/D#
                                       位在控制字节第 6 位 */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
    };
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel io 建不起来（%s）", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,        /* 模块上没有复位脚 */
        .bits_per_pixel = 1,
    };
    esp_lcd_panel_ssd1306_config_t ssd_cfg = {
        .height = OLED_HEIGHT,
    };
    panel_cfg.vendor_config = &ssd_cfg;

    err = esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ssd1306 panel 建不起来（%s）", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "屏初始化失败（大概率是接线/供电），跳过");
        return false;
    }
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_mirror(s_panel, OLED_MIRROR_X, OLED_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_ready = true;
    oled_clear();
    oled_flush();
    ESP_LOGI(TAG, "屏幕就绪：%dx%d @0x%02X（SDA=%d SCL=%d）",
             OLED_W, OLED_HEIGHT, OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
    return true;
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_HEIGHT) return;
    uint8_t *p = &s_fb[(y >> 3) * OLED_W + x];
    uint8_t bit = (uint8_t)(1u << (y & 7));
    if (on) *p |= bit;
    else    *p &= (uint8_t)~bit;
}

void oled_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void oled_flush(void)
{
    if (!s_ready) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_HEIGHT, s_fb);
}

void oled_fill(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            oled_pixel(i, j, on);
}

void oled_hline(int x, int y, int w) { oled_fill(x, y, w, 1, true); }
void oled_vline(int x, int y, int h) { oled_fill(x, y, 1, h, true); }

void oled_frame(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    oled_hline(x, y, w);
    oled_hline(x, y + h - 1, w);
    oled_vline(x, y, h);
    oled_vline(x + w - 1, y, h);
}

int oled_text_w(const char *s, int scale)
{
    if (!s) return 0;
    return (int)strlen(s) * FONT_ADV * scale;
}

void oled_text(int x, int y, const char *s, int scale, bool inv)
{
    if (!s || scale < 1) return;
    int pen = x;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = font5x7[c - 32];
        for (int col = 0; col < FONT_W; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < FONT_H; row++) {
                if (!(bits & (1u << row))) continue;
                if (scale == 1) {
                    oled_pixel(pen + col, y + row, !inv);
                } else {
                    oled_fill(pen + col * scale, y + row * scale, scale, scale, !inv);
                }
            }
        }
        pen += FONT_ADV * scale;
    }
}

#endif  /* OLED_ENABLE */
