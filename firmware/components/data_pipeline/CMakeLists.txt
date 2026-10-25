# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
  SRCS
    "src/types.cpp"
    "src/json_path.cpp"
    "src/source_plan.cpp"
    "src/parser.cpp"
    "src/history.cpp"
    "src/cache.cpp"
    "src/pipeline.cpp"
    "src/net_stack_transport.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    dashboard_core
    eego_hal
    net_stack
)
