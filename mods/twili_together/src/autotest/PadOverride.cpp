#include "autotest/AutoTest.hpp"
#include "autotest/State.hpp"

#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twili::autotest {

using namespace detail;

// Pad reads happen before mod_update counts the tick, hence >= below.
void overridePad(PADStatus* status) {
    State& s = state();
    if (!s.active || status == nullptr) {
        return;
    }
    PADStatus& pad = status[0];
    if (s.padActive && s.simTicks >= s.padEndTick) {
        s.padActive = false;
    }
    if (!s.padActive) {
        // Ignore the machine's real controller.
        std::memset(&pad, 0, sizeof(pad));
        pad.err = PAD_ERR_NONE;
        return;
    }
    float x = s.padStickX;
    float y = s.padStickY;
    if (s.padCircle) {
        const float angle = static_cast<float>(s.padFrame) * 0.02f;
        x = std::cos(angle);
        y = std::sin(angle);
    }
    s.padFrame++;
    std::memset(&pad, 0, sizeof(pad));
    pad.stickX = static_cast<s8>(std::lround(std::clamp(x, -1.0f, 1.0f) * 72.0f));
    pad.stickY = static_cast<s8>(std::lround(std::clamp(y, -1.0f, 1.0f) * 72.0f));
    pad.button = s.padButtons;
    pad.err = PAD_ERR_NONE;
}

void pulsePad(float stickX, float stickY, uint16_t buttons, int ticks) {
    State& s = state();
    s.padActive = true;
    s.padFrame = 0;
    s.padEndTick = s.simTicks + ticks;
    s.padStickX = stickX;
    s.padStickY = stickY;
    s.padCircle = false;
    s.padButtons = buttons;
}

bool padBusy() {
    return state().padActive;
}

}  // namespace twili::autotest
