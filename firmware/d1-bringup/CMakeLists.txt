# D1 上电自检 —— 只验证：能编译、能烧、能连串口、看得到 flash/PSRAM 的真实容量
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(d1_bringup)
