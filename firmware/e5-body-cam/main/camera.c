/* camera.c —— OV2640 眼睛（从 e4-ov2640 那份验证过的配置搬过来）

 * 为什么单独一个文件：
 *   main.c 已经 1000 多行（音频、屏幕、模式、MQTT），摄像头是**可选**能力，
 *   独立成文件之后，"这块板没接摄像头"只要 CAM_ENABLE 0 就行，音频照跑。
 *
 * 和 e4 的差别只有三点：
 *   ① 不抢 MQTT 客户端 —— 取到帧交给 main.c 发（同一条连接、同一个前缀）；
 *   ② 不常驻推流，默认只在收到 "CAM SNAP"/"CAM STREAM 1" 时出图；
 *   ③ 不占串口 —— 这块板的串口是调试日志，别插 JPEG 进去。
 */
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"

#include "config.h"
#include "camera.h"

static const char *TAG = "cam";
static bool s_ready;
static framesize_t s_size = CAM_SIZE_DEFAULT;
static int s_quality = CAM_JPEG_QUALITY;

bool eye_init(void)
{
#if !CAM_ENABLE
    ESP_LOGW(TAG, "CAM_ENABLE=0：跳过摄像头");
    return false;
#else
    camera_config_t c = {
        .pin_pwdn  = PIN_CAM_PWDN,
        .pin_reset = PIN_CAM_RESET,
        .pin_xclk  = CAM_HAS_OWN_OSC ? -1 : PIN_CAM_XCLK,

        .pin_sccb_sda = PIN_CAM_SIOD,
        .pin_sccb_scl = PIN_CAM_SIOC,

        .pin_d7 = PIN_CAM_D7,
        .pin_d6 = PIN_CAM_D6,
        .pin_d5 = PIN_CAM_D5,
        .pin_d4 = PIN_CAM_D4,
        .pin_d3 = PIN_CAM_D3,
        .pin_d2 = PIN_CAM_D2,
        .pin_d1 = PIN_CAM_D1,
        .pin_d0 = PIN_CAM_D0,

        .pin_vsync = PIN_CAM_VSYNC,
        .pin_href  = PIN_CAM_HREF,
        .pin_pclk  = PIN_CAM_PCLK,

        /* 模块自带 12MHz 有源晶振（Y1），所以 pin_xclk=-1：驱动整段跳过 LEDC。
         * 这里写的频率只在"模块没有晶振、由 ESP32 供时钟"时才有意义。 */
        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer   = LEDC_TIMER_1,      /* 别和 OLED/其他 LEDC 抢 TIMER_0 */
        .ledc_channel = LEDC_CHANNEL_4,

        .pixel_format = PIXFORMAT_JPEG,    /* OV2640 硬件出 JPEG，不占 CPU */
        .frame_size   = s_size,             /* 允许 CAM SIZE 改完重建时沿用新尺寸 */
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count     = CAM_FB_COUNT,      /* ≥2 个缓冲只有 JPEG 才允许 */
        .grab_mode    = CAMERA_GRAB_LATEST,
        .jpeg_buffer_size = 0,
    };

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头起不来（0x%x）—— 音频照跑，只是没有眼睛", err);
        if (err == ESP_ERR_NOT_FOUND || err == 0x105) {
            ESP_LOGE(TAG, "  0x105 = 读不到传感器：先量 VCC3.3、再看 SCL/SDA 有没有接反");
        } else if (err == 0x106) {
            ESP_LOGE(TAG, "  0x106 = PID 不对：这块模块可能不是 OV2640");
        }
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "传感器 PID=0x%02x（OV2640 应该是 0x26）", s->id.PID);
    }
    ESP_LOGI(TAG, "摄像头就绪：XCLK=%s，D0..D7=%d,%d,%d,%d,%d,%d,%d,%d，"
                  "PCLK=%d VSYNC=%d HREF=%d，SCCB=%d/%d，RESET=%d",
             CAM_HAS_OWN_OSC ? "模块自带晶振（不驱动）" : "ESP32 提供",
             PIN_CAM_D0, PIN_CAM_D1, PIN_CAM_D2, PIN_CAM_D3,
             PIN_CAM_D4, PIN_CAM_D5, PIN_CAM_D6, PIN_CAM_D7,
             PIN_CAM_PCLK, PIN_CAM_VSYNC, PIN_CAM_HREF,
             PIN_CAM_SIOC, PIN_CAM_SIOD, PIN_CAM_RESET);

    /* 预热两帧：第一帧曝光往往还没收敛，扔掉比发出去好看 */
    for (int i = 0; i < 2; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
    }

    s_ready = true;
    return true;
#endif
}

bool eye_ready(void)
{
    return s_ready;
}

camera_fb_t *eye_grab(void)
{
    if (!s_ready) return NULL;
    return esp_camera_fb_get();
}

void eye_release(camera_fb_t *fb)
{
    if (fb) esp_camera_fb_return(fb);
}

bool eye_set_size(const char *name)
{
    if (!s_ready) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_framesize) return false;

    framesize_t fs = CAM_SIZE_DEFAULT;
    if (strstr(name, "QVGA")) {
        fs = FRAMESIZE_QVGA;
    } else if (strstr(name, "SVGA")) {
        fs = FRAMESIZE_SVGA;
    } else if (strstr(name, "UXGA")) {
        fs = FRAMESIZE_UXGA;
    } else if (strstr(name, "VGA")) {
        fs = FRAMESIZE_VGA;
    } else {
        return false;
    }

    if (s->set_framesize(s, fs) != 0) {
        ESP_LOGW(TAG, "改分辨率失败（%s）", name);
        return false;
    }
    s_size = fs;

    /* ⚠️ 2026-10-31 实测：OV2640 在 JPEG 模式下**运行期**改分辨率，
     * set_framesize 会返回 0，但之后 cam_hal 一直 "Failed to get frame: timeout"，
     * 一帧都出不来（e4 上也是这样）。所以改完必须把整个驱动重来一遍。 */
    esp_camera_deinit();
    s_ready = false;
    if (!eye_init()) {
        ESP_LOGE(TAG, "改分辨率后重启摄像头失败");
        return false;
    }
    ESP_LOGI(TAG, "分辨率 => %s（已重建驱动）", name);
    return true;
}

int eye_set_quality(int q)
{
    if (!s_ready) return -1;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return -1;
    if (q < 0) q = 0;
    if (q > 63) q = 63;
    int old = s_quality;
    if (s->set_quality(s, q) != 0) return -1;
    s_quality = q;
    return old;
}

int eye_quality(void) { return s_quality; }

const char *eye_size_name(void)
{
    switch (s_size) {
    case FRAMESIZE_QVGA: return "QVGA 320x240";
    case FRAMESIZE_SVGA: return "SVGA 800x600";
    case FRAMESIZE_UXGA: return "UXGA 1600x1200";
    default:             return "VGA 640x480";
    }
}
