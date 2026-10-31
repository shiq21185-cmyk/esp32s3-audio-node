# e3-ref：把 llcircle/esp32s3_ov7670_wifi 的实现原样拿过来跑通，
# 只改三处：引脚（按我们这块面包板）、Wi-Fi 改 STA、TCP 换 UART。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e3_ref)
