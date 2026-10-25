# SPDX-License-Identifier: GPL-3.0-or-later
# Host-side (Linux/macOS g++/clang++) unit tests for the pure-logic firmware
# components. No ESP-IDF required:
#
#   cmake -S firmware/test/host -B build/host-tests
#   cmake --build build/host-tests -j
#   ctest --test-dir build/host-tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(eego_firmware_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(C "${FW_ROOT}/components")

option(EEGO_HOST_SANITIZE "Build host tests with ASan/UBSan" ON)
set(HT_WARN -Wall -Wextra)
if(EEGO_HOST_SANITIZE AND NOT MSVC)
  set(HT_SAN -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# Firmware sources under test, compiled unmodified. ESP-IDF headers that a few
# translation units include are replaced by tiny stubs in ./stubs.
set(FW_SOURCES
  ${C}/json_dom/src/json.cpp
  ${C}/alert_core/src/sha1.cpp
  ${C}/alert_core/src/types.cpp
  ${C}/alert_core/src/type_registry.cpp
  ${C}/alert_core/src/time_util.cpp
  ${C}/alert_core/src/ulid.cpp
  ${C}/alert_core/src/store.cpp
  ${C}/alert_core/src/persistence.cpp
  ${C}/alert_core/src/file_io_stdio.cpp
  ${C}/dashboard_core/src/model.cpp
  ${C}/dashboard_core/src/expression.cpp
  ${C}/dashboard_core/src/config.cpp
  ${C}/dashboard_core/src/serialize.cpp
  ${C}/dashboard_core/src/validate.cpp
  ${C}/dashboard_ui/src/geometry.cpp
  ${C}/dashboard_ui/src/layout.cpp
  ${C}/dashboard_ui/src/preview_data.cpp
  ${C}/data_pipeline/src/types.cpp
  ${C}/data_pipeline/src/json_path.cpp
  ${C}/data_pipeline/src/parser.cpp
  ${C}/data_pipeline/src/history.cpp
  ${C}/data_pipeline/src/source_plan.cpp
  ${C}/data_pipeline/src/cache.cpp
  ${C}/net_stack/src/credentials.cpp
  ${C}/device_settings/src/settings_model.cpp
  ${C}/eego_hal/input_gesture.cpp
  ${C}/eego_hal/atomic_file.cpp
  ${C}/admin/src/probe_mask.cpp
  ${C}/admin/src/input_binding_model.cpp
  ${FW_ROOT}/main/refresh_policy.cpp
  ${FW_ROOT}/main/battery_policy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/net_request_host.cpp
)
add_library(fw_under_test STATIC ${FW_SOURCES})
target_include_directories(fw_under_test PUBLIC
  ${C}/json_dom/include
  ${C}/alert_core/include
  ${C}/dashboard_core/include
  ${C}/dashboard_ui/include
  ${C}/data_pipeline/include
  ${C}/net_stack/include
  ${C}/device_settings/include
  ${C}/eego_hal/include
  ${C}/admin/include
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
# Pure-logic main/ units (refresh_policy, battery_policy) are compiled without the rest of main/.
target_include_directories(fw_under_test PRIVATE ${FW_ROOT}/main)
target_compile_options(fw_under_test PRIVATE ${HT_WARN} ${HT_SAN})
target_link_options(fw_under_test PUBLIC ${HT_SAN})

set(TEST_SUITES dashboard layout expression pipeline gesture settings probe_mask alert_store atomic_file
    refresh_policy battery_policy)
add_executable(firmware_host_tests
  test_main.cpp
  test_dashboard.cpp
  test_layout.cpp
  test_expression.cpp
  test_pipeline.cpp
  test_gesture.cpp
  test_settings.cpp
  test_probe_mask.cpp
  test_alert_store.cpp
  test_atomic_file.cpp
  test_refresh_policy.cpp
  test_battery_policy.cpp
)
target_include_directories(firmware_host_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_ROOT}/main  # refresh_policy.h
  ${C}/admin/src  # dashboard_templates.inc (generated preset library)
)
target_compile_definitions(firmware_host_tests PRIVATE EEGO_FIRMWARE_ROOT="${FW_ROOT}")
target_compile_options(firmware_host_tests PRIVATE ${HT_WARN} ${HT_SAN})
target_link_libraries(firmware_host_tests PRIVATE fw_under_test)

option(EEGO_HOST_RENDER "Build the dashboard PNG render harness (test/host/render)" OFF)
add_subdirectory(render)  # dashboard_ui_host (+ dashboard_render when EEGO_HOST_RENDER)

# dashboard_ui tests that need the real renderer, canvas and (fallback) font.
set(UI_TEST_SUITES alert_hit preview)
add_executable(firmware_ui_tests
  test_main.cpp
  test_alert_hit.cpp
  test_preview.cpp
)
target_include_directories(firmware_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(firmware_ui_tests PRIVATE EEGO_FIRMWARE_ROOT="${FW_ROOT}")
target_compile_options(firmware_ui_tests PRIVATE ${HT_WARN} ${HT_SAN})
target_link_libraries(firmware_ui_tests PRIVATE dashboard_ui_host)

enable_testing()
foreach(suite IN LISTS TEST_SUITES)
  add_test(NAME ${suite} COMMAND firmware_host_tests --suite ${suite})
endforeach()
foreach(suite IN LISTS UI_TEST_SUITES)
  add_test(NAME ${suite} COMMAND firmware_ui_tests --suite ${suite})
endforeach()
