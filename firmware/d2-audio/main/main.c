/*
 * D2 音频点名：I2S 输出（MAX98357 + 喇叭）+ I2S 输入（INMP441 麦克风）
 *
 * 引脚来自这块板（wangxun 语音板 v1.0）的原理图，不是文档里的旧表：
 *   MAX98357 : BCLK=IO16  LRC=IO17  DIN=IO15   GAIN 悬空=9dB, SD 悬空=常开
 *   INMP441  : SCK =IO4   WS =IO5   SD =IO6    L/R 接 GND => 数据在左声道
 *
 * 行为：蜂鸣 3 声 -> 安静收 1.5 秒并统计麦克风电平 -> 循环。
 *
 * 怎么判断结果：
 *   - 听到 440Hz 短促三声              => 功放/喇叭/I2S 输出/时钟全对
 *   - 峰值随你说话明显变大（安静 <200，说话 >1000）=> 麦克风通
 *   - 一直贴地（peak 只有几十、不随声音变）=> 麦克风没接或引脚/供电不对
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SR          16000          /* 采样率 */
#define TONE_HZ     1000
#define AMP         0.35f          /* 幅度 35%：确认低音量下也有余量，不是"只有满幅才响" */

#define PIN_AMP_BCLK 16
#define PIN_AMP_LRC  17
#define PIN_AMP_DIN  15
#define PIN_MIC_SCK  4
#define PIN_MIC_WS   5
#define PIN_MIC_SD   6

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

static void audio_init(void)
{
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_cfg.dma_desc_num = 6;
    tx_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx, NULL));

    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_AMP_BCLK,
            .ws   = PIN_AMP_LRC,
            .dout = PIN_AMP_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_cfg.dma_desc_num = 6;
    rx_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx));

    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_SD,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

/* 播 ms 毫秒的 440Hz 正弦；fade_ms 做淡入淡出，避免爆音 */
static void play_tone(int ms, int fade_ms)
{
    enum { CHUNK = 256 };
    int16_t buf[CHUNK * 2];
    float phase = 0.0f;
    const float dphase = 2.0f * (float)M_PI * (float)TONE_HZ / (float)SR;
    const int total = SR * ms / 1000;
    const int fade = SR * fade_ms / 1000;
    int done = 0;

    while (done < total) {
        int n = (total - done < CHUNK) ? (total - done) : CHUNK;
        for (int i = 0; i < n; i++) {
            float env = 1.0f;
            int idx = done + i;
            if (fade > 0) {
                if (idx < fade) {
                    env = (float)idx / (float)fade;
                } else if (idx > total - fade) {
                    env = (float)(total - idx) / (float)fade;
                }
            }
            int16_t v = (int16_t)(sinf(phase) * AMP * env * 32767.0f);
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = v;
            phase += dphase;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        done += n;
    }
}

/*
 * 关掉声音不能只是"不再写数据"。
 *
 * ESP-IDF 的 I2S 发送是环形 DMA：应用不喂新数据时，硬件会一直重复播放
 * 描述符里最后那段数据 —— 于是最后一个音会被无限循环，听起来就是"一直在叫"。
 * 所以静音必须显式写零，把这段"尾巴"从 DMA 里冲干净。
 */
static void play_silence(int ms)
{
    enum { CHUNK = 256 };
    int16_t zero[CHUNK * 2];
    memset(zero, 0, sizeof(zero));

    int total = SR * ms / 1000;
    while (total > 0) {
        int n = (total < CHUNK) ? total : CHUNK;
        size_t written = 0;
        i2s_channel_write(s_tx, zero, (size_t)n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        total -= n;
    }
}

/* 读 ms 毫秒麦克风，返回左声道的峰值与 RMS */
static void mic_measure(int ms, int *peak_out, float *rms_out)
{
    enum { CHUNK = 256 };
    int16_t buf[CHUNK * 2];
    int peak = 0;
    double sumsq = 0.0;
    long count = 0;
    const int total = SR * ms / 1000;
    int done = 0;

    while (done < total) {
        int want = (total - done < CHUNK) ? (total - done) : CHUNK;
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, (size_t)want * 2 * sizeof(int16_t), &got,
                                         pdMS_TO_TICKS(500));
        if (err != ESP_OK || got == 0) {
            break;
        }
        int frames = (int)(got / (2 * sizeof(int16_t)));
        for (int i = 0; i < frames; i++) {
            int v = buf[i * 2];         /* 左声道：INMP441 的 L/R 接地 */
            int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
            sumsq += (double)v * (double)v;
            count++;
        }
        done += frames;
    }

    *peak_out = peak;
    *rms_out = count ? (float)sqrt(sumsq / (double)count) : 0.0f;
}

void app_main(void)
{
    printf("\n==================================================\n");
    printf("  小鲸鱼身体 · D2 音频点名\n");
    printf("==================================================\n");
    printf("  功放 MAX98357 : BCLK=IO%d  LRC=IO%d  DIN=IO%d\n", PIN_AMP_BCLK, PIN_AMP_LRC,
           PIN_AMP_DIN);
    printf("  麦克 INMP441  : SCK =IO%d  WS =IO%d  SD =IO%d\n", PIN_MIC_SCK, PIN_MIC_WS,
           PIN_MIC_SD);
    printf("  采样率        : %d Hz, 16bit 立体声\n", SR);
    printf("--------------------------------------------------\n");
    printf("  每轮：蜂鸣 3 声，然后收 1.5 秒麦克风电平\n");
    printf("  安静时 peak 通常 < 200；对着板子说话 peak 应 > 1000\n");
    printf("==================================================\n\n");

    audio_init();

    int round = 0;
    while (1) {
        /* 三声短鸣：150ms 响 + 850ms 停。
         * 停的那段必须写零，不能靠 vTaskDelay —— 否则 DMA 会把最后一个音循环播放。 */
        for (int i = 0; i < 3; i++) {
            play_tone(150, 15);
            play_silence(850);
        }
        play_silence(1500);
        vTaskDelay(pdMS_TO_TICKS(200));

        int peak = 0;
        float rms = 0.0f;
        mic_measure(1500, &peak, &rms);

        printf("[round %3d]  mic peak=%5d  rms=%8.1f  %s\n", round++, peak, rms,
               peak > 1000   ? "有语音"
               : peak > 200  ? "有环境声"
                             : "很安静/可能没信号");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
