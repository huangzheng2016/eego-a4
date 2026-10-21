# EEGO A4 告警/数据面板固件 — ESP-IDF v5.5.1 工程
#
# 纯 ESP-IDF（无 Arduino 依赖）。组件依赖方向（自上而下，禁止反向）：
#   main → board → eego_hal
#   main → eego_ui → eego_hal
#   eego_ui → third_party/lodepng
#
# 本目录代码为 GPL-3.0 衍生作品（移植自 eego-a4-weread，GPL-3.0）；
# 第三方库 lodepng 保持 zlib 许可，见 components/third_party/lodepng/。
cmake_minimum_required(VERSION 3.16.0)

# 第三方库组件目录（lodepng 等自带独立 CMakeLists 的库）
set(EXTRA_COMPONENT_DIRS "components/third_party")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eego-a4-alert-panel)
