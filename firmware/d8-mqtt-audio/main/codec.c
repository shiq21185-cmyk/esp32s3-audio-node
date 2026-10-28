/* codec.c —— 板子自己编解码 Opus（用乐鑫的 esp_audio_codec 预编译库）
 *
 * 为什么要在板子上做：
 *   裸 PCM 16k/16bit/单声道 = 32 KB/s = 115 MB/小时。带出门用手机热点、
 *   走公网 broker（EMQX 免费额度 1GB/月），上下行一起几个小时就吃穿了。
 *   Opus 16 kbps ≈ 2 KB/s，同样额度能讲一百多小时。
 *
 * 格式（两端必须完全一致，改一处就要改另一处）：
 *   16000 Hz / 单声道 / 16bit / **60 ms 一帧（960 样本）** / 16000 bps CBR / VOIP
 *   一帧编码出来固定 **120 字节**（16000 × 0.06 / 8），所以每个 MQTT 包就一个帧。
 *
 * 为什么选 60ms 而不是 Opus 常用的 20ms：
 *   60ms 是原来 PCM 的包节奏（960 帧一包），改动最小；Opus 的算法延迟对
 *   "聊天机器人"完全无所谓（她本来就要想好几秒）。
 *
 * ⚠️ 解码器那边 self_delimited 必须和编码器一致 —— 我们一个包正好一个帧，
 *    所以两边都是 false。
 */

#include <string.h>

#include "esp_log.h"
#include "esp_audio_types.h"
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

#include "codec.h"

static const char *TAG = "codec";

static void *s_enc;
static void *s_dec;
static int s_enc_in;            /* 一帧输入字节数（1920） */
static int s_enc_out;           /* 一帧输出缓冲区建议大小 */

bool codec_init(void)
{
    /* ---------------- 编码器 ---------------- */
    esp_opus_enc_config_t ec = {
        .sample_rate      = CODEC_SAMPLE_RATE,
        .channel          = 1,                  /* 单声道 */
        .bits_per_sample  = 16,
        .bitrate          = CODEC_BITRATE,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,   /* 优先"听得清" */
        .complexity       = 1,                  /* 0 最省 CPU；3 在小板子上仍富裕 */
        .enable_fec       = false,              /* 不用带内纠错：丢包靠 PLC 兜 */
        .enable_dtx       = false,              /* 关掉不连续发送，保持包节奏稳定 */
        .enable_vbr       = false,              /* CBR：包大小恒定，好估算流量 */
    };
    esp_audio_err_t err = esp_opus_enc_open(&ec, sizeof(ec), &s_enc);
    if (err != ESP_AUDIO_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "Opus 编码器打开失败: %d", (int)err);
        return false;
    }
    err = esp_opus_enc_get_frame_size(s_enc, &s_enc_in, &s_enc_out);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "取编码器帧大小失败: %d", (int)err);
        return false;
    }
    if (s_enc_in != CODEC_FRAME_BYTES) {
        ESP_LOGW(TAG, "编码器一帧要 %d 字节，而我们的块是 %d 字节（不影响，能凑整就行）",
                 s_enc_in, CODEC_FRAME_BYTES);
    }

    /* ---------------- 解码器 ---------------- */
    esp_opus_dec_cfg_t dc = {
        .sample_rate    = CODEC_SAMPLE_RATE,
        .channel        = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    err = esp_opus_dec_open(&dc, sizeof(dc), &s_dec);
    if (err != ESP_AUDIO_ERR_OK || !s_dec) {
        ESP_LOGE(TAG, "Opus 解码器打开失败: %d", (int)err);
        return false;
    }

    ESP_LOGI(TAG, "Opus 就绪：%d Hz 单声道 %d kbps，一帧输入 %d 字节、输出缓冲区 %d 字节",
             CODEC_SAMPLE_RATE, CODEC_BITRATE / 1000, s_enc_in, s_enc_out);
    return true;
}

int codec_encode(const int16_t *pcm, int samples, uint8_t *out, int out_cap)
{
    if (!s_enc || samples <= 0) return -1;
    int in_bytes = samples * 2;
    if (in_bytes != s_enc_in) return -1;      /* 只能是整帧，零头由调用方补齐 */

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len    = (uint32_t)in_bytes,
    };
    esp_audio_enc_out_frame_t o = {
        .buffer = out,
        .len    = (uint32_t)out_cap,
    };
    esp_audio_err_t err = esp_opus_enc_process(s_enc, &in, &o);
    if (err != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)o.encoded_bytes;
}

int codec_decode(const uint8_t *in, int len, int16_t *out, int out_samples)
{
    if (!s_dec || len <= 0) return -1;

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)in,     /* 库不写这块，只是签名没加 const */
        .len    = (uint32_t)len,
    };
    esp_audio_dec_out_frame_t o = {
        .buffer = (uint8_t *)out,
        .len    = (uint32_t)(out_samples * 2),
    };
    esp_audio_dec_info_t info;
    esp_audio_err_t err = esp_opus_dec_decode(s_dec, &raw, &o, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        return -1;
    }
    return (int)(o.decoded_size / 2);        /* 返回样本数（不是字节） */
}
