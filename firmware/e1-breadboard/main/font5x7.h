/* 5x7 点阵表（字符 32..126）。数据在 font5x7.c，由 .tmp/genfont.py 生成。 */
#pragma once

#include <stdint.h>

#define FONT_W 5
#define FONT_H 7
/* 字距：一列空白，所以一个字占 6 像素；行距同理，一行占 8 像素。 */
#define FONT_ADV 6

extern const uint8_t font5x7[95][5];
