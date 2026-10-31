cmake_minimum_required(VERSION 3.16)

# 乐鑫那个 esp_audio_codec 组件（含 Opus 预编译静态库）有 46 MB，
# 没必要在仓库里存两份，这里直接指到 d8 那份去。
# ⚠️ 代价：这个工程不能单独拎出 rot 目录构建（除非把组件一起带走）。
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../d8-mqtt-audio/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e5_body_cam)
