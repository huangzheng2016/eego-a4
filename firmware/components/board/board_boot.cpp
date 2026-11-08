// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "board/board_boot.h"

#include "eego_hal.h"

namespace eego {
namespace board {
BootStatus beginStandardHardware(Uc8279cDisplay& display, Gslx680Touch& touch,
                                 const BootOptions& options) {
    BootStatus st;

    PowerManager::instance().holdPower();
    st.powerLatched = true;

    st.touchReady = options.touch && (touch.begin(I2C_NUM_0) == ESP_OK);
    st.buttonsReady = (ButtonManager::instance().begin() == ESP_OK);

    st.frontlightPresent = FrontlightManager::instance().lm3630aChipPresent();

    PowerManager::instance().isCharging();

    Pcf8563Rtc rtc(I2C_NUM_0);
    st.rtcReady = (rtc.begin() == ESP_OK);

    st.sdReady = (SdCard::instance().mount() == ESP_OK);

    st.displayReady = (display.begin(options.deferDisplay) == ESP_OK);
    if (st.displayReady && options.clearDisplay && !options.deferDisplay) {
        display.clear();
        display.display(RefreshMode::Full);
        display.waitRefreshComplete();
    }

    return st;
}

}  // namespace board
}  // namespace eego
