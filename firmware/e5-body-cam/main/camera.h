/* camera.h —— 摄像头对 main.c 暴露的全部接口。
 *
 * 设计原则：**取帧的人负责把帧还回来**（esp32-camera 的 fb 是池子里的，
 * 不还就再也没得取）。main.c 里只有两处取帧：拍照和推流，两处都在
 * 同一个函数里配对了 eye_release()，不要在别处零散调用 eye_grab()。 */
#pragma once

#include <stdbool.h>
#include "esp_camera.h"

/* 起摄像头。返回 false 不致命：音频/MQTT/屏幕照常跑，只是没有眼睛。 */
bool eye_init(void);
bool eye_ready(void);

/* 取一帧 JPEG。失败返回 NULL。用完必须 eye_release()。 */
camera_fb_t *eye_grab(void);
void eye_release(camera_fb_t *fb);

/* "VGA" / "QVGA" / "SVGA" / "UXGA"（字符串里含这几个词就认）。 */
bool eye_set_size(const char *name);
const char *eye_size_name(void);

/* JPEG 质量 0~63（越小越清晰、越大越省流量）。返回旧值，-1 表示失败。 */
int eye_set_quality(int q);
int eye_quality(void);
