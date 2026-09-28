// Where the synced flags say a player may be; outside it, "Catch up to story" repairs.

#include "story/StoryTypes.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <cstring>

namespace twili::story {
namespace {

// Captured and taken to the castle: Midna has not warped Link out (M_014), Faron not restored.
bool capturedActive() {
    return (dComIfGs_isTransformLV(0) || dComIfGs_isEventBit(dSv_event_flag_c::F_0630)) &&
           !dComIfGs_isEventBit(dSv_event_flag_c::M_014) && !dComIfGs_isDarkClearLV(0);
}

// Ordon and Faron in twilight until the Faron spirit is revived.
bool ordonTwilightActive() {
    return dComIfGs_isEventBit(dSv_event_flag_c::M_014) && !dComIfGs_isDarkClearLV(0);
}

// The escape runs through the sewers, rooftops and Zelda's tower: all R_SP107.
constexpr const char* kCapturedStages[] = {"R_SP107", nullptr};
constexpr const char* kOrdonTwilightStages[] = {
    "F_SP103", "F_SP104", "F_SP00", "R_SP01", "F_SP108", "R_SP108", "D_SB10", nullptr};

Entrance entrance(const char* stage, int8_t room, int16_t point) {
    Entrance e;
    e.setStage(stage);
    e.room = room;
    e.point = point;
    return e;
}

const StorySegment kSegments[] = {
    // Point 24, the wake-up: transform level 0, and with F_0630 the cell loads layer 14.
    {"captured", "you were captured and taken to Hyrule Castle", &capturedActive, kCapturedStages,
        entrance("R_SP107", 0, 24), Form::Wolf, "the Hyrule Castle prison cell"},
    // No canonical entrance yet: catch-up teleports to a teammate standing there instead.
    {"ordon-twilight", "Midna brought you back to twilight Ordon and Faron", &ordonTwilightActive,
        kOrdonTwilightStages, Entrance{}, Form::Wolf, "Ordon"},
};

}  // namespace

const StorySegment* activeSegment() {
    for (const StorySegment& seg : kSegments) {
        if (seg.active()) {
            return &seg;
        }
    }
    return nullptr;
}

const StorySegment* segmentById(const std::string& id) {
    for (const StorySegment& seg : kSegments) {
        if (id == seg.id) {
            return &seg;
        }
    }
    return nullptr;
}

bool segmentAllows(const StorySegment& seg, const char* stage) {
    if (seg.allowedStages == nullptr) {
        return true;
    }
    for (const char* const* s = seg.allowedStages; *s != nullptr; s++) {
        if (std::strncmp(*s, stage, 8) == 0) {
            return true;
        }
    }
    return false;
}

bool predictSpawnWolf(const Entrance& e) {
    // Transforming unlocked: the form is the player's own choice.
    if (dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return dComIfGs_getTransformStatus() != 0;
    }
    const dSv_player_status_b_c& b = dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusB();
    uint8_t tlv = b.mTransformLevelFlag;
    uint8_t dcl = b.mDarkClearLevelFlag;
    const auto at = [&](const char* stage, int room) {
        return std::strncmp(e.stage, stage, 8) == 0 && e.room == room;
    };
    // The entrance's own phase_1 effects.
    if (at("F_SP108", 1) && e.point == 3) {
        dcl |= 1 << 0;
    }
    if (at("F_SP109", 0) && e.point == 30) {
        dcl |= 1 << 1;
    }
    if (at("F_SP115", 1) && e.point == 20) {
        dcl |= 1 << 2;
    }
    if (at("F_SP117", 1) && e.point == 99) {
        dcl |= 1 << 3;
    }
    if (at("R_SP107", 0) && (e.point == 0 || e.point == 24)) {
        tlv |= 1 << 0;
    }
    if (at("F_SP121", 2) && e.point == 10) {
        tlv |= 1 << 1;
    }
    if (at("F_SP121", 9) && e.point == 10) {
        tlv |= 1 << 2;
    }
    if (at("F_SP121", 10) && (e.point == 20 || e.point == 23)) {
        tlv |= 1 << 3;
    }
    for (int i = 0; i < 4; i++) {
        // Eldin and Lanayru stay human until their save switch is set; follow never goes there.
        if ((tlv & (1 << i)) != 0 && (dcl & (1 << i)) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace twili::story
