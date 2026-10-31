#pragma once
#include <stdint.h>

/* 直接读写 OV7670 的寄存器（绕过 esp32-camera）。
 *
 * 为什么需要这个：esp32-camera 的 ov7670 驱动**没有实现** sensor_t 的
 * set_reg / get_reg（sensors/ov7670.c 里根本没赋值，是 NULL 指针，
 * 直接调会崩）。而调寄存器正是排查"每行只有一个采样值"唯一可用的手段
 * —— 不能回读、不能改位，就只能整机重烧，一轮十分钟。
 *
 * 做法：SCCB 走的是 IDF 新 I2C 驱动（driver/sccb-ng.c），总线在
 * I2C_NUM_1 上（sdkconfig 里 CONFIG_SCCB_HARDWARE_I2C_PORT1=y）。
 * 我们不去动那条总线，只是把它的 handle 取出来，再挂一个 0x21 的
 * 设备上去 —— 和驱动用的是同一个 bus，不会打架。
 */
int  cam_reg_init(void);                 /* 0 = 成功 */
int  cam_reg_write(uint8_t reg, uint8_t val);
int  cam_reg_read(uint8_t reg, uint8_t *out);
