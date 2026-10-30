/* 屏幕 UI 对外只有两个接口。真值从 main.c 里取（见 ui_start 的快照回调）。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool wifi_ok;      /* 连上热点、拿到 IP */
    bool mqtt_ok;      /* broker 已连 */
    bool mic_on;       /* 麦克风在采（板子侧开关） */
    bool gate_open;    /* 人声门限此刻判定"有人说话" */
    bool btn_window;   /* 随身模式：按键开出来的送话窗口还在 */
    bool speaking;     /* 喇叭正在出声（缓冲有料或刚写完） */
    int  rms;          /* 最近一包音量 */
    int  floor;        /* 房间底噪估计 */
    int  mode;         /* 0 = 室内门限（gate），1 = 随身按键（button） */
    uint32_t up_pkts;  /* 上行包数（累计） */
    uint32_t down_pkts;/* 下行包数（累计） */
    char ip[16];
} ui_status_t;

/* 传进来的回调每 100ms 被调一次，要在里面把上面那堆值填好。
 * 之所以用回调而不是全局变量：main.c 里那些都是 static，
 * 暴露成 extern 等于把固件的内部状态变成公共 API，以后改名全是坑。 */
void ui_start(ui_status_t (*snapshot)(void));
