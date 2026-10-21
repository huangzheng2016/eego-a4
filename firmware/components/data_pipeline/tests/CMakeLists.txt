# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(data_pipeline_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMPONENTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(test_data_pipeline
  test_data_pipeline.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/types.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/json_path.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/source_plan.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/parser.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/history.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/cache.cpp
  ${COMPONENTS_DIR}/data_pipeline/src/pipeline.cpp
  ${COMPONENTS_DIR}/dashboard_core/src/json.cpp
  ${COMPONENTS_DIR}/dashboard_core/src/model.cpp
  ${COMPONENTS_DIR}/dashboard_core/src/expression.cpp
)
target_include_directories(test_data_pipeline PRIVATE
  ${COMPONENTS_DIR}/data_pipeline/include
  ${COMPONENTS_DIR}/dashboard_core/include
  ${COMPONENTS_DIR}/net_stack/include
)
target_compile_options(test_data_pipeline PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME data_pipeline_host_tests COMMAND test_data_pipeline)
