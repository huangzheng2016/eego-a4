# SPDX-License-Identifier: GPL-3.0-or-later
# Pure C++ dashboard domain model: no UI, network, alert_core, or ESP-IDF APIs.
idf_component_register(
  SRCS
    "src/json.cpp"
    "src/model.cpp"
    "src/expression.cpp"
    "src/config.cpp"
    "src/migration.cpp"
    "src/serialize.cpp"
    "src/validate.cpp"
    "src/snapshot.cpp"
  INCLUDE_DIRS "include"
)
