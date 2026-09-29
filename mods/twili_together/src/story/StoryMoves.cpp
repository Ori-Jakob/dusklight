// The curated moves, the phase_1 side-effect entrances and the qualification of a move.

#include "story/StoryTypes.hpp"

#include "core/LocalPlayer.hpp"

#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace twili::story {

// Labels and follower recipes, not gates: an unmatched move still qualifies on its effects.
const StoryMoveDef kStoryMoves[] = {
    // The capture: arrival demos ending in the cell; followers load point 24, the wake-up.
    {"faron-capture", "{name} was captured", "the Hyrule Castle prison cell", "F_SP108", -1,
        "R_SP107", 0, nullptr, 24, Form::Wolf, false, true},
    // Midna warps Link from the castle to twilight Ordon (M_014).
    {"castle-escape", "{name} escaped Hyrule Castle with Midna", "Ordon", "R_SP107", -1, nullptr,
        -1, nullptr, -1, Form::Wolf, false, false, -1, -1, 0x0502},
    // Twilight gate: F_SP108 point 23 room 0 layer 10.
    {"faron-gate", "{name} passed the twilight gate with Midna", "Faron Woods in twilight",
        "F_SP108", -1, "F_SP108", 0, "TW_GATE_FILONE", 23, Form::Wolf, true, false},
    // kytag04's warp to the Faron Spring (room 1 point 3 clears twilight level 0).
    {"faron-light", "{name} restored the Faron Spring", "the Faron Spring", nullptr, -1, "F_SP108",
        1, nullptr, -1, Form::Human, false, false, -1, fpcNm_KYTAG04_e},
    // Twilight walls: the arrival at point 10 plays the transformation unless its switch is on.
    {"eldin-gate", "{name} passed the Eldin twilight wall with Midna", "Eldin in twilight",
        nullptr, -1, "F_SP121", 2, nullptr, 10, Form::Any, false, false},
    {"lanayru-gate", "{name} passed the Lanayru twilight wall with Midna", "Lanayru in twilight",
        nullptr, -1, "F_SP121", 9, nullptr, 10, Form::Any, false, false},
    // kytag04's warp to the spring (point 30, layer 8); followers load where its scenes end.
    {"eldin-light", "{name} restored the Eldin Spring", "Kakariko Village", nullptr, -1, "F_SP109",
        -1, nullptr, -1, Form::Human, false, false, -1, fpcNm_KYTAG04_e},
    {"lanayru-light", "{name} restored the Lanayru Spring", "Lake Hylia", nullptr, -1, "F_SP115",
        -1, nullptr, -1, Form::Human, false, false, -1, fpcNm_KYTAG04_e},
    // After the Lakebed, Zant: Hyrule Field room 10, transform level 3.
    {"mdh-start", "{name} was cursed by Zant", "Hyrule Field", nullptr, -1, "F_SP121", 10,
        nullptr, -1, Form::Wolf, false, false, 20},
    {"mdh-start", "{name} was cursed by Zant", "Hyrule Field", nullptr, -1, "F_SP121", 10,
        nullptr, -1, Form::Wolf, false, false, 23},
    // Zelda heals Midna in the castle tower (F_0250).
    {"mdh-zelda", "{name}'s Midna was healed by Zelda", "Faron Woods", "R_SP107", -1, nullptr, -1,
        nullptr, -1, Form::Wolf, false, false, -1, -1, 0x1E08},
    // The pedestal (F_0264) sends Link to point 99 (twilight level 3 clears); the scene ends on 150.
    {"master-sword", "{name} drew the Master Sword", "the Sacred Grove", nullptr, -1, "F_SP117",
        1, nullptr, -1, Form::Human, false, false, -1, -1, 0x2020},
    {"palace-entry", "{name} entered the Palace of Twilight", "the Palace of Twilight", "F_SP125",
        -1, "D_MN08", -1, nullptr, -1, Form::Any, false, false},
    // b_zant loads the throne room point 25 on cutscene layer 9; its scene ends on point 23.
    {"zant-defeated", "{name} defeated Zant", "the Palace of Twilight", nullptr, -1, "D_MN08A", 10,
        nullptr, -1, Form::Any, false, false, -1, fpcNm_B_ZANT_e},
    {"castle-barrier", "{name} shattered the barrier around Hyrule Castle", "Hyrule Castle",
        "F_SP116", -1, nullptr, -1, nullptr, -1, Form::Any, false, false, -1, -1, 0x4208},
    {"final-battle", "{name} went on to face Ganondorf", "Hyrule Castle", nullptr, -1, "D_MN09A",
        -1, nullptr, -1, Form::Any, false, false},
    {"final-battle", "{name} went on to face Ganondorf", "Hyrule Field", nullptr, -1, "D_MN09B",
        -1, nullptr, -1, Form::Any, false, false},
    {"final-battle", "{name} went on to face Ganondorf", "Hyrule Field", nullptr, -1, "D_MN09C",
        -1, nullptr, -1, Form::Any, false, false},
};
const int kStoryMoveCount = static_cast<int>(sizeof(kStoryMoves) / sizeof(kStoryMoves[0]));

namespace {

struct SidePoint {
    const char* stage;
    int8_t room;
    int16_t point;
};

// d_s_play.cpp phase_1: entrances whose load changes saved story state.
constexpr SidePoint kSideEffectPoints[] = {
    {"F_SP108", 1, 3},
    {"F_SP109", 0, 30},
    {"F_SP115", 1, 20},
    {"F_SP117", 1, 99},
    {"R_SP107", 0, 0},
    {"R_SP107", 0, 24},
    {"F_SP121", 2, 10},
    {"F_SP121", 9, 10},
    {"F_SP121", 10, 20},
    {"F_SP121", 10, 23},
    {"F_SP104", 1, 23},
    {"D_MN08D", 50, 20},
};

// Event requesters whose relocations are transports, minigames or repeatable rides.
constexpr int16_t kNotStoryRequesters[] = {
    fpcNm_NPC_GWOLF_e,  // golden wolf
    fpcNm_NPC_KN_e,     // Hero's Shade
    fpcNm_NPC_FAIRY_e,  // Cave of Ordeals
    fpcNm_MG_ROD_e,     // fishing
    fpcNm_NPC_HENNA_e,  // fishing hole
    fpcNm_NPC_YKW_e,    // Yeto's snowboard race
    fpcNm_NPC_TARO_e,   // Talo sends Link back
};

struct BossStage {
    const char* stage;
    const char* dungeon;
};

// daObjBossWarp_c's levels.
constexpr BossStage kBossStages[] = {
    {"D_MN05A", "the Forest Temple"},
    {"D_MN04A", "the Goron Mines"},
    {"D_MN01A", "the Lakebed Temple"},
    {"D_MN10A", "the Arbiter's Grounds"},
    {"D_MN11A", "the Snowpeak Ruins"},
    {"D_MN06A", "the Temple of Time"},
    {"D_MN07A", "the City in the Sky"},
    {"D_MN08A", "the Palace of Twilight"},
};

// Event names whose relocation is never story; grows from logs.
constexpr const char* kNotStory[] = {nullptr};

bool stageIs(const char* want, const char* have) {
    return want == nullptr || std::strncmp(want, have, 8) == 0;
}

std::string fixedString(const char* s, size_t n) {
    return std::string(s, strnlen(s, n));
}

}  // namespace

void Entrance::setStage(const char* name) {
    std::memset(stage, 0, sizeof(stage));
    if (name != nullptr) {
        std::strncpy(stage, name, sizeof(stage) - 1);
    }
}

bool Entrance::sameStageRoom(const char* otherStage, int otherRoom) const {
    return otherStage != nullptr && std::strncmp(stage, otherStage, sizeof(stage)) == 0 &&
           room == otherRoom;
}

nlohmann::json Entrance::toJson() const {
    return {{"stage", fixedString(stage, sizeof(stage))}, {"room", room}, {"point", point},
        {"layerArg", layerArg}, {"layer", layer}};
}

Entrance Entrance::fromJson(const nlohmann::json& j) {
    Entrance e;
    if (!j.is_object()) {
        return e;
    }
    const std::string s = j.value("stage", std::string{});
    if (s.size() <= 7) {
        e.setStage(s.c_str());
    }
    const int room = j.value("room", -1);
    const int point = j.value("point", -1);
    const int layerArg = j.value("layerArg", -1);
    const int layer = j.value("layer", -1);
    e.room = static_cast<int8_t>(room >= 0 && room < 64 ? room : -1);
    e.point = static_cast<int16_t>(point >= 0 && point < 256 ? point : -1);
    e.layerArg = static_cast<int8_t>(layerArg >= 0 && layerArg < 15 ? layerArg : -1);
    e.layer = static_cast<int8_t>(layer >= 0 && layer < 15 ? layer : -1);
    return e;
}

nlohmann::json Hop::toJson() const {
    return {{"at", at.toJson()}, {"m", arrivalMap}, {"sw", arrivalSwitch}, {"name", arrivalName}};
}

Hop Hop::fromJson(const nlohmann::json& j) {
    Hop h;
    if (!j.is_object()) {
        return h;
    }
    if (const auto it = j.find("at"); it != j.end()) {
        h.at = Entrance::fromJson(*it);
    }
    h.arrivalMap = static_cast<uint8_t>(j.value("m", 0xFF) & 0xFF);
    h.arrivalSwitch = static_cast<uint8_t>(j.value("sw", 0xFF) & 0xFF);
    h.arrivalName = j.value("name", std::string{}).substr(0, 32);
    return h;
}

const char* reqKindName(ReqKind kind) {
    switch (kind) {
    case ReqKind::Player:
        return "player";
    case ReqKind::Tag:
        return "tag";
    case ReqKind::Actor:
        return "actor";
    default:
        return "none";
    }
}

nlohmann::json EventRef::toJson() const {
    return {{"name", name}, {"ev", eventId}, {"m", mapToolId}, {"lt", listType}, {"type", mapType},
        {"sw", switchNo}, {"req", requester}, {"rk", static_cast<int>(reqKind)}, {"mode", mode},
        {"arrival", arrivalDemo}};
}

EventRef EventRef::fromJson(const nlohmann::json& j) {
    EventRef e;
    if (!j.is_object()) {
        return e;
    }
    e.name = j.value("name", std::string{}).substr(0, 32);
    e.eventId = static_cast<int16_t>(j.value("ev", -1));
    e.mapToolId = static_cast<uint8_t>(j.value("m", 0xFF) & 0xFF);
    e.listType = static_cast<uint8_t>(j.value("lt", 0) & 0xFF);
    e.mapType = static_cast<uint8_t>(j.value("type", 0xFF) & 0xFF);
    e.switchNo = static_cast<uint8_t>(j.value("sw", 0xFF) & 0xFF);
    e.requester = static_cast<int16_t>(j.value("req", -1));
    const int rk = j.value("rk", 0);
    e.reqKind = rk >= 0 && rk <= static_cast<int>(ReqKind::Actor) ? static_cast<ReqKind>(rk) :
                                                                   ReqKind::None;
    e.mode = static_cast<uint8_t>(j.value("mode", 0) & 0xFF);
    e.arrivalDemo = j.value("arrival", false);
    return e;
}

nlohmann::json MoveRecord::toJson() const {
    nlohmann::json hl = nlohmann::json::array();
    for (const Hop& h : hopList) {
        hl.push_back(h.toJson());
    }
    return {
        {"mid", id},
        {"from", from.toJson()},
        {"to", to.toJson()},
        {"event", event.toJson()},
        {"wolf", {wolfBefore, wolfAfter}},
        {"tlv", {tlvBefore, tlvAfter}},
        {"dcl", {dclBefore, dclAfter}},
        {"arrivalEvent", {{"m", arrivalEvent}, {"name", arrivalName}}},
        {"curated", curated >= 0 ? kStoryMoves[curated].id : ""},
        {"qual", qual},
        {"hops", hops},
        {"hl", std::move(hl)},
        {"bits", bits},
        {"key", key},
        {"th", transientHint},
        {"boss", bossDefeated},
    };
}

MoveRecord MoveRecord::fromJson(const nlohmann::json& j) {
    MoveRecord m;
    m.id = j.value("mid", std::string{}).substr(0, 32);
    if (const auto it = j.find("from"); it != j.end()) {
        m.from = Entrance::fromJson(*it);
    }
    if (const auto it = j.find("to"); it != j.end()) {
        m.to = Entrance::fromJson(*it);
    }
    if (const auto it = j.find("event"); it != j.end()) {
        m.event = EventRef::fromJson(*it);
    }
    const auto pair = [&](const char* key, auto& before, auto& after) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array() || it->size() != 2) {
            return;
        }
        using T = std::decay_t<decltype(before)>;
        if constexpr (std::is_same_v<T, bool>) {
            before = (*it)[0].is_boolean() && (*it)[0].template get<bool>();
            after = (*it)[1].is_boolean() && (*it)[1].template get<bool>();
        } else {
            before =
                static_cast<T>((*it)[0].is_number_integer() ? (*it)[0].template get<int>() : 0);
            after = static_cast<T>((*it)[1].is_number_integer() ? (*it)[1].template get<int>() : 0);
        }
    };
    pair("wolf", m.wolfBefore, m.wolfAfter);
    pair("tlv", m.tlvBefore, m.tlvAfter);
    pair("dcl", m.dclBefore, m.dclAfter);
    if (const auto it = j.find("arrivalEvent"); it != j.end() && it->is_object()) {
        m.arrivalEvent = static_cast<uint8_t>(it->value("m", 0xFF) & 0xFF);
        m.arrivalName = it->value("name", std::string{}).substr(0, 32);
    }
    const std::string curated = j.value("curated", std::string{});
    for (int i = 0; i < kStoryMoveCount; i++) {
        if (curated == kStoryMoves[i].id) {
            m.curated = i;
        }
    }
    m.qual = j.value("qual", 0u);
    m.hops = j.value("hops", 1);
    if (const auto it = j.find("hl"); it != j.end() && it->is_array()) {
        for (const auto& h : *it) {
            if (m.hopList.size() < kMaxHopRecords) {
                m.hopList.push_back(Hop::fromJson(h));
            }
        }
    }
    if (const auto it = j.find("bits"); it != j.end() && it->is_array()) {
        for (const auto& b : *it) {
            if (b.is_number_unsigned() && b.get<unsigned>() <= 0xFFFF &&
                m.bits.size() < kMaxMoveBits)
            {
                m.bits.push_back(static_cast<uint16_t>(b.get<unsigned>()));
            }
        }
    }
    m.key = j.value("key", std::string{}).substr(0, 96);
    m.transientHint = j.value("th", false);
    m.bossDefeated = j.value("boss", false);
    return m;
}

const char* formName(Form form) {
    switch (form) {
    case Form::Human:
        return "human";
    case Form::Wolf:
        return "wolf";
    default:
        return "any";
    }
}

const char* catchUpKindName(CatchUpPlan::Kind kind) {
    switch (kind) {
    case CatchUpPlan::Kind::Entrance:
        return "entrance";
    case CatchUpPlan::Kind::TeleportToPlayer:
        return "teleport";
    default:
        return "none";
    }
}

const char* loadPhaseName(LoadPhase phase) {
    switch (phase) {
    case LoadPhase::Waiting:
        return "waiting";
    case LoadPhase::Loading:
        return "loading";
    case LoadPhase::Arrived:
        return "arrived";
    case LoadPhase::Failed:
        return "failed";
    default:
        return "idle";
    }
}

const char* promptKindName(PromptKind kind) {
    switch (kind) {
    case PromptKind::Move:
        return "move";
    case PromptKind::Inconsistent:
        return "inconsistent";
    default:
        return "none";
    }
}

const char* joinStateName(JoinState state) {
    switch (state) {
    case JoinState::Waiting:
        return "waiting";
    case JoinState::Ordered:
        return "ordered";
    case JoinState::Running:
        return "running";
    case JoinState::Missed:
        return "missed";
    case JoinState::Shared:
        return "shared";
    case JoinState::Ended:
        return "ended";
    default:
        return "none";
    }
}

int matchCuratedMove(const MoveRecord& m) {
    for (int i = 0; i < kStoryMoveCount; i++) {
        const StoryMoveDef& d = kStoryMoves[i];
        if (!stageIs(d.fromStage, m.from.stage) || (d.fromRoom >= 0 && d.fromRoom != m.from.room)) {
            continue;
        }
        if (!stageIs(d.toStage, m.to.stage) || (d.toRoom >= 0 && d.toRoom != m.to.room)) {
            continue;
        }
        // "Anywhere" means another stage: the capture's hops inside the cell are no escape.
        if (d.toStage == nullptr && std::strncmp(m.from.stage, m.to.stage, 8) == 0) {
            continue;
        }
        if (d.eventPrefix != nullptr &&
            m.event.name.compare(0, std::strlen(d.eventPrefix), d.eventPrefix) != 0)
        {
            continue;
        }
        // A row naming only a destination point needs that point.
        if (d.followPoint >= 0 && d.eventPrefix == nullptr && d.fromStage == nullptr &&
            m.to.point != d.followPoint)
        {
            continue;
        }
        if ((d.toPoint >= 0 && m.to.point != d.toPoint) ||
            (d.requester >= 0 && m.event.requester != d.requester) ||
            (d.bit != 0 && std::find(m.bits.begin(), m.bits.end(), d.bit) == m.bits.end()))
        {
            continue;
        }
        return i;
    }
    return -1;
}

std::string moveKey(const MoveRecord& m) {
    const std::string what =
        m.event.name.empty() ? "req:" + std::to_string(m.event.requester) : m.event.name;
    return std::string(reqKindName(m.event.reqKind)) + "|" + what + "|" +
           fixedString(m.from.stage, sizeof(m.from.stage)) + "/" + std::to_string(m.from.room) +
           "|" + fixedString(m.to.stage, sizeof(m.to.stage)) + "/" + std::to_string(m.to.room);
}

const StoryMoveDef* curatedMove(int index) {
    return index >= 0 && index < kStoryMoveCount ? &kStoryMoves[index] : nullptr;
}

bool isSideEffectPoint(const char* stage, int room, int point) {
    for (const SidePoint& p : kSideEffectPoints) {
        if (std::strncmp(p.stage, stage, 8) == 0 && p.room == room && p.point == point) {
            return true;
        }
    }
    return false;
}

bool isNotStory(const std::string& eventName) {
    for (const char* name : kNotStory) {
        if (name != nullptr && eventName == name) {
            return true;
        }
    }
    return false;
}

bool isNotStoryRequester(int16_t profile) {
    return profile >= 0 && std::find(std::begin(kNotStoryRequesters),
                               std::end(kNotStoryRequesters), profile) != std::end(kNotStoryRequesters);
}

const char* bossStageDungeon(const char* stage) {
    for (const BossStage& b : kBossStages) {
        if (std::strncmp(b.stage, stage, 8) == 0) {
            return b.dungeon;
        }
    }
    return nullptr;
}

uint32_t qualify(const MoveRecord& m, uint32_t localBitsDuring) {
    if (isNotStory(m.event.name) || isNotStoryRequester(m.event.requester)) {
        return 0;
    }
    uint32_t q = 0;
    if (m.curated >= 0) {
        q |= kQualCurated;
    }
    if (isSideEffectPoint(m.to.stage, m.to.room, m.to.point)) {
        q |= kQualSidePoint;
    }
    if (m.wolfBefore != m.wolfAfter) {
        q |= kQualForm;
    }
    if (m.tlvBefore != m.tlvAfter || m.dclBefore != m.dclAfter) {
        q |= kQualLevels;
    }
    if (m.event.switchNo != 0xFF) {
        q |= kQualOneShot;
    }
    if (localBitsDuring > 0) {
        q |= kQualStoryBits;
    }
    if (m.bossDefeated && bossStageDungeon(m.from.stage) != nullptr &&
        std::strncmp(m.from.stage, m.to.stage, 8) != 0)
    {
        q |= kQualBoss;
    }
    return q;
}

std::string placeName(const MoveRecord& m) {
    if (const StoryMoveDef* d = curatedMove(m.curated)) {
        return d->place;
    }
    return local::mapName(m.to.stage, m.to.room);
}

}  // namespace twili::story
