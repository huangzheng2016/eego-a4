# SPDX-License-Identifier: GPL-3.0-or-later
# alert_core — pure C++ alert domain core (M12 architecture).
#
# Requires only json_dom (pure C++) and has no ESP-IDF/UI/network includes: the
# component must compile for the host (unit verification, mock tools) and for
# the firmware unchanged. Platform IO arrives via IAlertFileIo implementations
# wired by the board/net components.

idf_component_register(
  SRCS
    "src/sha1.cpp"
    "src/ulid.cpp"
    "src/time_util.cpp"
    "src/types.cpp"
    "src/type_registry.cpp"
    "src/store.cpp"
    "src/snapshot.cpp"
    "src/persistence.cpp"
    "src/file_io_stdio.cpp"
  INCLUDE_DIRS "include"
  REQUIRES json_dom
)
