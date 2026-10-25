// SPDX-License-Identifier: GPL-3.0-or-later
// Host render-harness stub of the ESP32-S3 ROM tjpgd API: JPEG is unsupported
// on the host (jd_prepare always fails); PNG goes through lodepng as on device.
#pragma once
#include <cstdint>
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef enum { JDR_OK = 0, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2, JDR_PAR, JDR_FMT1, JDR_FMT2, JDR_FMT3 } JRESULT;
typedef struct { WORD left, right, top, bottom; } JRECT;
typedef struct JDEC {
  WORD width, height;
  void* device;
} JDEC;
inline JRESULT jd_prepare(JDEC*, UINT (*)(JDEC*, BYTE*, UINT), void*, UINT, void*) { return JDR_FMT3; }
inline JRESULT jd_decomp(JDEC*, UINT (*)(JDEC*, void*, JRECT*), BYTE) { return JDR_FMT3; }
