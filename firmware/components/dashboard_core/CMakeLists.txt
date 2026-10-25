# SPDX-License-Identifier: GPL-3.0-or-later
# Pure C++ dashboard domain model: no UI, network, alert_core, or ESP-IDF APIs.
idf_component_register(
  SRCS
    "src/model.cpp"
    "src/expression.cpp"
    "src/config.cpp"
    "src/serialize.cpp"
    "src/validate.cpp"
  INCLUDE_DIRS "include"
  REQUIRES json_dom
)
