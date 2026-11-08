# SPDX-License-Identifier: GPL-3.0-or-later
# lodepng: single-file PNG decoder (zlib license, Copyright (c) 2005-2026 Lode Vandevenne)
# Upstream: https://github.com/lvandeve/lodepng (version 20260119)
# Altered from upstream (each change is marked in the source):
#   1. Trimmed: LODEPNG_NO_COMPILE_DISK / LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#   2. Allocator uses ESP-IDF heap_caps (PSRAM first, internal RAM fallback)
# The license notice at the top of lodepng.cpp / lodepng.h must not be removed.
idf_component_register(
    SRCS "lodepng.cpp"
    INCLUDE_DIRS "."
    REQUIRES heap
)
