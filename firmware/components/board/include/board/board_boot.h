// SPDX-License-Identifier: GPL-3.0-or-later
//
//
//
#pragma once

#include <esp_err.h>

namespace eego {
class Uc8279cDisplay;
class Gslx680Touch;

namespace board {
struct BootStatus {
    bool powerLatched = false;
    bool touchReady = false;
    bool buttonsReady = false;
    bool frontlightPresent = false;
    bool rtcReady = false;
    bool sdReady = false;
    bool displayReady = false;
};

struct BootOptions {
    bool touch = true;
    bool clearDisplay = true;
    bool deferDisplay = false;
};

BootStatus beginStandardHardware(Uc8279cDisplay& display, Gslx680Touch& touch,
                                 const BootOptions& options = BootOptions{});

}  // namespace board
}  // namespace eego
