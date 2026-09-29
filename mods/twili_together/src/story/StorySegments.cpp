// Where the synced flags say a player may be; outside it, "Catch up to story" repairs.

#include "story/StoryTypes.hpp"

#include "core/LocalPlayer.hpp"
#include "story/StoryLog.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <cstring>

namespace twili::story {
namespace {

bool bit(uint16_t no) {
    return dComIfGs_isEventBit(no) != FALSE;
}

bool twilight(int level) {
    return dComIfGs_isTransformLV(level) && !dComIfGs_isDarkClearLV(level);
}

// Captured and taken to the castle: Midna has not warped Link out (M_014), Faron not restored.
bool capturedActive() {
    return (dComIfGs_isTransformLV(0) || bit(dSv_event_flag_c::F_0630)) &&
           !bit(dSv_event_flag_c::M_014) && !dComIfGs_isDarkClearLV(0);
}

// Ordon and Faron in twilight until the Faron spirit is revived.
bool ordonTwilightActive() {
    return bit(dSv_event_flag_c::M_014) && !dComIfGs_isDarkClearLV(0);
}

// Zant's curse (M_071) until Zelda heals Midna (F_0250).
bool mdhActive() {
    return bit(dSv_event_flag_c::M_071) && !bit(dSv_event_flag_c::F_0250) && twilight(3);
}

// Healed, still a wolf until the Master Sword.
bool wolfUntilSwordActive() {
    return bit(dSv_event_flag_c::F_0250) && twilight(3);
}

bool lanayruTwilightActive() {
    return twilight(2);
}

bool eldinTwilightActive() {
    return twilight(1);
}

// The escape runs through the sewers, rooftops and Zelda's tower: all R_SP107.
constexpr const char* kCapturedStages[] = {"R_SP107", nullptr};
constexpr const char* kOrdonTwilightStages[] = {
    "F_SP103", "F_SP104", "F_SP00", "R_SP01", "F_SP108", "R_SP108", "D_SB10", nullptr};
constexpr const char* kMdhCore[] = {
    "F_SP121", "F_SP122", "F_SP116", "R_SP116", "R_SP107", nullptr};
constexpr const char* kMdhAllowed[] = {
    "F_SP121", "F_SP122", "F_SP116", "R_SP116", "R_SP107", "F_SP115", "R_SP160", nullptr};
constexpr const char* kWolfUntilSwordCore[] = {"F_SP108", "F_SP117", nullptr};
constexpr const char* kWolfUntilSwordAllowed[] = {"F_SP108", "F_SP117", "R_SP108", "D_SB10",
    "F_SP121", "F_SP122", "F_SP116", "R_SP116", "R_SP107", nullptr};
// l_darkworld_tbl's Lanayru stages (R_SP107 clears with Lanayru), and the field.
constexpr const char* kLanayruCore[] = {"F_SP112", "F_SP113", "F_SP115", "F_SP116", "F_SP122",
    "F_SP126", "R_SP116", "R_SP107", "F_SP121", nullptr};
// Portals reach the cleared provinces; their dungeons stay open to a wolf.
constexpr const char* kLanayruAllowed[] = {"F_SP112", "F_SP113", "F_SP115", "F_SP116", "F_SP122",
    "F_SP126", "R_SP116", "R_SP107", "F_SP121", "F_SP109", "F_SP110", "F_SP111", "R_SP109",
    "R_SP110", "R_SP209", "F_SP108", "R_SP108", "D_SB10", "F_SP117", "F_SP103", "F_SP104", "F_SP00",
    "R_SP01", "D_MN05", "D_MN05A", "D_MN05B", "D_MN04", "D_MN04A", "D_MN04B", nullptr};
constexpr const char* kEldinCore[] = {
    "F_SP109", "F_SP110", "R_SP109", "F_SP111", "R_SP209", "F_SP121", nullptr};
constexpr const char* kEldinAllowed[] = {"F_SP109", "F_SP110", "R_SP109", "F_SP111", "R_SP209",
    "F_SP121", "F_SP108", "R_SP108", "D_SB10", "F_SP117", "F_SP103", "F_SP104", "F_SP00", "R_SP01",
    "D_MN05", "D_MN05A", "D_MN05B", nullptr};

Entrance entrance(const char* stage, int8_t room, int16_t point) {
    Entrance e;
    e.setStage(stage);
    e.room = room;
    e.point = point;
    return e;
}

// Most specific first: activeSegment() returns the first whose predicate holds.
const StorySegment kSegments[] = {
    // Point 24, the wake-up: transform level 0, and with F_0630 the cell loads layer 14.
    {"captured", "you were captured and taken to Hyrule Castle", &capturedActive, kCapturedStages,
        kCapturedStages, entrance("R_SP107", 0, 24), Form::Wolf, "the Hyrule Castle prison cell"},
    // No canonical entrance: the learned escape, else teleport to a teammate standing there.
    {"ordon-twilight", "Midna brought you back to twilight Ordon and Faron", &ordonTwilightActive,
        kOrdonTwilightStages, kOrdonTwilightStages, Entrance{}, Form::Wolf, "Ordon",
        "castle-escape"},
    {"mdh", "Zant cursed Midna and you are to carry her to Hyrule Castle", &mdhActive, kMdhCore,
        kMdhAllowed, entrance("F_SP121", 10, 20), Form::Wolf, "Hyrule Field", "mdh-start"},
    {"wolf-until-sword", "Zelda saved Midna; you stay a wolf until the Master Sword",
        &wolfUntilSwordActive, kWolfUntilSwordCore, kWolfUntilSwordAllowed, Entrance{}, Form::Wolf,
        "Faron Woods", "mdh-zelda"},
    // The twilight walls' own destinations.
    {"lanayru-twilight", "you entered Lanayru in twilight", &lanayruTwilightActive, kLanayruCore,
        kLanayruAllowed, entrance("F_SP121", 9, 10), Form::Wolf, "Lanayru in twilight"},
    {"eldin-twilight", "you entered Eldin in twilight", &eldinTwilightActive, kEldinCore,
        kEldinAllowed, entrance("F_SP121", 2, 10), Form::Wolf, "Eldin in twilight"},
};

bool listHas(const char* const* list, const char* stage) {
    if (list == nullptr) {
        return true;
    }
    for (const char* const* s = list; *s != nullptr; s++) {
        if (std::strncmp(*s, stage, 8) == 0) {
            return true;
        }
    }
    return false;
}

struct StageTbl {
    const char* prefix;
    int8_t tbl;
};

// dStage_stagInfo_GetSaveTbl of each stage; a dungeon prefix covers its boss and miniboss rooms.
constexpr StageTbl kStageTbls[] = {
    {"F_SP00", 0x0}, {"F_SP103", 0x0}, {"F_SP104", 0x0}, {"R_SP01", 0x0}, {"R_SP107", 0x1},
    {"D_SB10", 0x2}, {"F_SP108", 0x2}, {"R_SP108", 0x2}, {"F_SP109", 0x3}, {"F_SP110", 0x3},
    {"F_SP111", 0x3}, {"F_SP128", 0x3}, {"R_SP109", 0x3}, {"R_SP110", 0x3}, {"R_SP128", 0x3},
    {"R_SP209", 0x3}, {"F_SP112", 0x4}, {"F_SP113", 0x4}, {"F_SP115", 0x4}, {"F_SP126", 0x4},
    {"R_SP127", 0x4}, {"F_SP121", 0x6}, {"F_SP122", 0x6}, {"F_SP123", 0x6}, {"F_SP200", 0x6},
    {"F_SP117", 0x7}, {"F_SP114", 0x8}, {"F_SP116", 0x9}, {"R_SP116", 0x9}, {"R_SP160", 0x9},
    {"R_SP161", 0x9}, {"F_SP118", 0xA}, {"F_SP124", 0xA}, {"F_SP125", 0xA}, {"F_SP127", 0xB},
    {"D_MN05", 0x10}, {"D_MN04", 0x11}, {"D_MN01", 0x12}, {"D_MN10", 0x13}, {"D_MN11", 0x14},
    {"D_MN06", 0x15}, {"D_MN07", 0x16}, {"D_MN08", 0x17}, {"D_MN09", 0x18}, {"D_SB00", 0x19},
    {"D_SB01", 0x19}, {"D_SB02", 0x19}, {"D_SB03", 0x1A}, {"D_SB04", 0x1A}, {"D_SB05", 0x1B},
    {"D_SB06", 0x1B}, {"D_SB07", 0x1B}, {"D_SB08", 0x1B}, {"D_SB09", 0x1B},
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
    return listHas(seg.allowedStages, stage);
}

SegmentState segmentState(const StorySegment& seg, const char* stage) {
    if (!listHas(seg.allowedStages, stage)) {
        return SegmentState::Inconsistent;
    }
    return listHas(seg.coreStages, stage) ? SegmentState::Consistent : SegmentState::Behind;
}

const char* segmentStateName(SegmentState state) {
    switch (state) {
    case SegmentState::Consistent:
        return "consistent";
    case SegmentState::Behind:
        return "behind";
    case SegmentState::Inconsistent:
        return "inconsistent";
    default:
        return "none";
    }
}

Entrance segmentEntrance(const StorySegment& seg, const MoveRecord** learned) {
    if (learned != nullptr) {
        *learned = nullptr;
    }
    if (seg.canonicalMove != nullptr) {
        if (const storylog::Learned* e = storylog::learnedByCurated(seg.canonicalMove)) {
            const Entrance& to = e->move.to;
            if (to.valid() && local::isKnownEntrance(to.stage, to.room, to.point)) {
                if (learned != nullptr) {
                    *learned = &e->move;
                }
                Entrance out = to;
                out.layerArg = -1;
                return out;
            }
        }
    }
    return seg.canonical;
}

int stageSaveTbl(const char* stage) {
    for (const StageTbl& t : kStageTbls) {
        const size_t n = std::strlen(t.prefix);
        const bool dungeon = t.prefix[0] == 'D' && t.prefix[2] == 'M';
        if (std::strncmp(stage, t.prefix, n) == 0 && (dungeon || stage[n] == '\0')) {
            return t.tbl;
        }
    }
    return -1;
}

int saveSwitch(const char* stage, int no) {
    const int tbl = stageSaveTbl(stage);
    if (tbl < 0 || no < 0 || no >= 128) {
        return -1;
    }
    return dComIfGs_isStageSwitch(tbl, no) != FALSE ? 1 : 0;
}

bool gateHumanArrival(const Entrance& e) {
    if (std::strncmp(e.stage, "F_SP121", 8) != 0 || e.point != 10) {
        return false;
    }
    // dComIfGs_Wolf_Change_Check: the wall's arrival shows the change until its switch is on.
    return (e.room == 2 && saveSwitch("F_SP121", 12) == 0) ||
           (e.room == 9 && saveSwitch("F_SP121", 13) == 0);
}

bool predictSpawnWolf(const Entrance& e) {
    // Transforming unlocked: the form is the player's own choice.
    if (dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return dComIfGs_getTransformStatus() != 0;
    }
    if (gateHumanArrival(e)) {
        return false;
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
        if ((tlv & (1 << i)) != 0 && (dcl & (1 << i)) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace twili::story
