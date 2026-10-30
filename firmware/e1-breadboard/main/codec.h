/* codec.h —— 板子上的 Opus 编解码（一进一出，都是 60ms 一帧的裸包） */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 与 config.h 里的音频格式保持同一套数（改一处要一起看） */
#define CODEC_SAMPLE_RATE   16000
#define CODEC_FRAME_MS      60
#define CODEC_FRAME_SAMPLES (CODEC_SAMPLE_RATE * CODEC_FRAME_MS / 1000)   /* 960 */
#define CODEC_FRAME_BYTES   (CODEC_FRAME_SAMPLES * 2)                     /* 1920 PCM */
#define CODEC_BITRATE       16000                                         /* bps */
#define CODEC_PKT_MAX       400      /* 一帧 Opus 输出上限，实测 120，留足余量 */

/* 打开编解码器。返回 false = 这个固件在这块板子上用不了（看串口日志）。 */
bool codec_init(void);

/* 把一整帧 PCM（samples 必须等于 CODEC_FRAME_SAMPLES）编成 Opus。
 * 返回编码后字节数；<0 = 失败。 */
int codec_encode(const int16_t *pcm, int samples, uint8_t *out, int out_cap);

/* 一个 Opus 裸包 → PCM。返回解出的**样本数**；<0 = 失败/坏包。 */
int codec_decode(const uint8_t *in, int len, int16_t *out, int out_samples);
