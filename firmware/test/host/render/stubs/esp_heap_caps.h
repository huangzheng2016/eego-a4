// SPDX-License-Identifier: GPL-3.0-or-later
// Host render-harness stub: heap_caps maps to the C heap.
#pragma once
#include <cstddef>
#include <cstdlib>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
inline void* heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void* heap_caps_realloc(void* ptr, size_t size, unsigned) { return std::realloc(ptr, size); }
inline void heap_caps_free(void* ptr) { std::free(ptr); }
