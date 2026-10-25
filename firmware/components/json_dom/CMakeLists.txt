# SPDX-License-Identifier: GPL-3.0-or-later
# json_dom — the single JSON DOM/parser/writer (pure C++, no REQUIRES).
# alert_core and dashboard_core re-export it under their own namespaces.
idf_component_register(
  SRCS
    "src/json.cpp"
  INCLUDE_DIRS "include"
)
