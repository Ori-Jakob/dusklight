#pragma once

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <cstdint>

namespace twili::sync {

// Event bits that mirror this machine's actors or UI: never sent, applied or merged.
inline constexpr uint16_t kLocalOnlyEventBits[] = {
    // Palace of Twilight: the room each Sol was left in (the carried ball is not synced).
    dSv_event_flag_c::F_0311,
    dSv_event_flag_c::F_0312,
    dSv_event_flag_c::F_0313,
    dSv_event_flag_c::F_0314,
    dSv_event_flag_c::F_0315,
    dSv_event_flag_c::F_0316,
    dSv_event_flag_c::F_0317,
    dSv_event_flag_c::F_0318,
    dSv_event_flag_c::F_0319,
    dSv_event_flag_c::F_0320,
    // Area-map show/hide, written on every map toggle and stage unload.
    dSv_event_flag_c::MAP_VISIBLE,
    // Fyrus and Dangoro fight state; F_0255/F_0257 flip every frame while Fyrus is stunned.
    dSv_event_flag_c::F_0254,
    dSv_event_flag_c::F_0255,
    dSv_event_flag_c::F_0256,
    dSv_event_flag_c::F_0257,
    dSv_event_flag_c::F_0669,
    dSv_event_flag_c::F_0670,
    dSv_event_flag_c::F_0671,
    dSv_event_flag_c::F_0672,
};

// The local-only bits within dSv_event_c::mEvent[byte].
constexpr uint8_t localOnlyEventMask(int byte) {
    uint8_t mask = 0;
    for (const uint16_t no : kLocalOnlyEventBits) {
        if ((no >> 8) == byte) {
            mask |= static_cast<uint8_t>(no);
        }
    }
    return mask;
}

constexpr bool isLocalOnlyEventBit(uint16_t no) {
    return (localOnlyEventMask(no >> 8) & static_cast<uint8_t>(no)) != 0;
}

}  // namespace twili::sync
