# SPDX-License-Identifier: GPL-3.0-or-later
#
#   main → board → eego_hal
#   main → eego_ui → eego_hal
#   eego_ui → third_party/lodepng
#
cmake_minimum_required(VERSION 3.16.0)

set(EXTRA_COMPONENT_DIRS "components/third_party")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" AND
   "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults" IS_NEWER_THAN "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
  message(STATUS "sdkconfig.defaults changed: regenerating sdkconfig")
  file(REMOVE "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eego-a4-alert-panel)
