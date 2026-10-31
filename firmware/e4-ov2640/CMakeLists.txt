# e4-ov2640：OV2640 + esp32-camera，UART 推 JPEG。
# 从 e3-ref 复制过来（e3 是照抄 llcircle/esp32s3_ov7670_wifi 的 OV7670 工程），
# 只换传感器配置和帧格式：RGB565 裸帧 -> 传感器硬件 JPEG。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e4_ov2640)
