// Steps for story sync; reference in the runner README.

#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"

#include "autotest/State.hpp"
#include "core/Base64.hpp"
#include "core/Config.hpp"
#include "core/Host.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "story/StoryLog.hpp"
#include "story/StoryNpc.hpp"
#include "story/StoryState.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "sync/WorldSync.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_obj_bosswarp.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_item.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "dolphin/pad.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace twili::autotest {

int stageSaveTbl(const char* stage) {
    return story::stageSaveTbl(stage);
}

namespace {
nlohmann::json sRemap;
std::string sRemapStage;
}  // namespace

void remapStageRequest(const char*& stage, int16_t& point, int8_t& room, int8_t& layer) {
    if (sRemap.is_null() || stage == nullptr) {
        return;
    }
    const nlohmann::json& from = sRemap["from"];
    const nlohmann::json& to = sRemap["to"];
    if (std::strncmp(stage, from.value("stage", std::string{}).c_str(), 8) != 0 ||
        (from.contains("room") && room != from.value("room", -1)) ||
        (from.contains("point") && point != from.value("point", -1)))
    {
        return;
    }
    TwiliLog.info("[autotest] remapping {}/{}/{} -> {}/{}/{}", stage, room, point,
        to.value("stage", std::string{}), to.value("room", 0), to.value("point", 0));
    sRemapStage = to.value("stage", std::string{});
    stage = sRemapStage.c_str();
    room = static_cast<int8_t>(to.value("room", 0));
    point = static_cast<int16_t>(to.value("point", 0));
    layer = static_cast<int8_t>(to.value("layer", -1));
}

// The story fields of `start`, applied after dComIfGs_init and before the first load.
void applyStoryStart(const nlohmann::json& start) {
    using nlohmann::json;
    // A captured save first; the synthetic fields apply on top.
    if (const auto it = start.find("fixtureData"); it != start.end() && it->is_string()) {
        std::vector<uint8_t> bytes;
        if (base64::decode(it->get<std::string>(), bytes) && bytes.size() == sizeof(dSv_save_c)) {
            std::memcpy(dComIfGs_getSaveData(), bytes.data(), bytes.size());
            TwiliLog.info("[autotest] start: fixture save loaded ({} bytes)", bytes.size());
        } else {
            TwiliLog.warn("[autotest] start: fixture save unusable ({} bytes, want {})",
                bytes.size(), sizeof(dSv_save_c));
        }
    }
    for (const auto& sw : start.value("saveSwitches", json::array())) {
        const int tbl = stageSaveTbl(sw.value("stage", std::string{}).c_str());
        if (tbl >= 0) {
            dComIfGs_onStageSwitch(tbl, sw.value("no", 0));
        }
    }
    for (const auto& item : start.value("items", json::array())) {
        if (item.is_number_integer()) {
            execItemGet(static_cast<u8>(item.get<int>()));
            dComIfGs_onItemFirstBit(static_cast<u8>(item.get<int>()));
        }
    }
    if (const auto it = start.find("collect"); it != start.end() && it->is_object()) {
        for (const auto& n : it->value("crystal", json::array())) {
            dComIfGs_onCollectCrystal(static_cast<u8>(n.get<int>()));
        }
        for (const auto& n : it->value("mirror", json::array())) {
            dComIfGs_onCollectMirror(static_cast<u8>(n.get<int>()));
        }
    }
    if (const auto it = start.find("lightDrops"); it != start.end() && it->is_object()) {
        for (const auto& [area, num] : it->items()) {
            dComIfGs_setLightDropNum(static_cast<u8>(std::stoi(area)), static_cast<u8>(num.get<int>()));
        }
    }
    for (const auto& area : start.value("vessels", json::array())) {
        dComIfGs_onLightDropGetFlag(static_cast<u8>(area.get<int>()));
    }
    if (start.contains("transformStatus")) {
        dComIfGs_setTransformStatus(start.value("transformStatus", std::string{}) == "wolf" ? 1 : 0);
    }
    for (const auto& stage : start.value("stageBoss", json::array())) {
        const int tbl = stageSaveTbl(stage.get<std::string>().c_str());
        // Not dComIfGs_onStageBossEnemy: it reads the stage info, null before the first load.
        if (tbl >= 0) {
            dComIfGs_getSaveData()->getSave(tbl).getBit().onStageBossEnemy();
        }
    }
    if (start.contains("midna")) {
        if (start.value("midna", std::string{}) == "ride") {
            dComIfGs_onEventBit(dSv_event_flag_c::M_067);
        } else {
            dComIfGs_offEventBit(dSv_event_flag_c::M_067);
        }
    }
}

namespace {

using nlohmann::json;
namespace sd = story::detail;

json sForcedMove;
int sForcedRetry = 0;
bool sSaveReqSeen = false;
int sTearsAtMark = 0;
int sTearTbox = -1;
int sTearCountBefore = 0;
bool sTearPicked = false;
int sSavePulse = 0;
int sSavePulseTick = 0;

const char* currentStage() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

bool matchesAny(const json& want, const std::string& got) {
    if (want.is_string()) {
        return want.get<std::string>() == got;
    }
    if (want.is_array()) {
        for (const auto& w : want) {
            if (w.is_string() && w.get<std::string>() == got) {
                return true;
            }
        }
    }
    return false;
}

struct TagFind {
    uint8_t eventNo;
};

void* findTag(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (fopAcM_GetName(actor) != fpcNm_TAG_EVENT_e) {
        return nullptr;
    }
    auto* tag = static_cast<daTag_Event_c*>(actor);
    return tag->getEventNo() == static_cast<TagFind*>(data)->eventNo ? actor : nullptr;
}

story::Entrance entrance(const char* stage, int room, int point) {
    story::Entrance e;
    e.setStage(stage);
    e.room = static_cast<int8_t>(room);
    e.point = static_cast<int16_t>(point);
    return e;
}

// The pure parts of story sync on synthetic data; the first failure, "" if none.
std::string selfTest() {
    std::string failure;
    const auto check = [&](bool ok, const std::string& what) {
        if (!ok && failure.empty()) {
            failure = what;
        }
    };

    story::MoveRecord m;
    m.from = entrance("F_SP108", 0, 0);
    m.to = entrance("R_SP107", 0, 24);
    const int capture = story::matchCuratedMove(m);
    check(capture >= 0 && std::strcmp(story::kStoryMoves[capture].id, "faron-capture") == 0,
        "F_SP108 -> R_SP107 room 0 is not faron-capture");
    m.to = entrance("F_SP108", 1, 3);
    m.from = entrance("F_SP117", 3, 0);
    check(story::matchCuratedMove(m) < 0, "faron-light matched without kytag04");
    m.event.requester = fpcNm_KYTAG04_e;
    m.event.reqKind = story::ReqKind::Actor;
    const int light = story::matchCuratedMove(m);
    check(light >= 0 && std::strcmp(story::kStoryMoves[light].id, "faron-light") == 0,
        "kytag04 -> F_SP108 room 1 is not faron-light");
    m.from = entrance("F_SP109", 0, 0);
    m.to = entrance("F_SP109", 0, 33);
    const int eldin = story::matchCuratedMove(m);
    check(eldin >= 0 && std::strcmp(story::kStoryMoves[eldin].id, "eldin-light") == 0,
        "kytag04 -> F_SP109 is not eldin-light");
    m.event.requester = -1;
    m.event.reqKind = story::ReqKind::None;
    m.from = entrance("D_MN01A", 50, 0);
    m.to = entrance("F_SP121", 10, 20);
    const int mdh = story::matchCuratedMove(m);
    check(mdh >= 0 && std::strcmp(story::kStoryMoves[mdh].id, "mdh-start") == 0,
        "-> F_SP121 room 10 point 20 is not mdh-start");
    m.from = entrance("R_SP107", 3, 0);
    m.to = entrance("F_SP103", 0, 0);
    check(story::matchCuratedMove(m) < 0, "castle-escape matched without M_014");
    m.bits = {0x0502};
    const int escape = story::matchCuratedMove(m);
    check(escape >= 0 && std::strcmp(story::kStoryMoves[escape].id, "castle-escape") == 0,
        "R_SP107 -> Ordon with M_014 is not castle-escape");
    m.bits.clear();
    m.from = entrance("F_SP121", 6, 0);
    m.to = entrance("D_MN05", 0, 0);
    check(story::matchCuratedMove(m) < 0, "field -> forest temple matched a curated move");

    m.curated = -1;
    m.event.switchNo = 0xFF;
    check(story::qualify(m, 0) == 0, "a plain move qualified");
    check(story::qualify(m, 1) == story::kQualStoryBits, "story bits did not qualify");
    m.event.switchNo = 12;
    check(story::qualify(m, 0) == story::kQualOneShot, "a one-shot event did not qualify");
    m.event.switchNo = 0xFF;
    m.wolfAfter = true;
    check((story::qualify(m, 0) & story::kQualForm) != 0, "a form change did not qualify");
    m.wolfAfter = false;
    m.tlvAfter = 1;
    check((story::qualify(m, 0) & story::kQualLevels) != 0, "a level change did not qualify");
    m.tlvAfter = 0;
    m.from = entrance("D_MN05A", 50, 0);
    m.to = entrance("F_SP108", 1, 0);
    m.bossDefeated = true;
    check(story::qualify(m, 0) == story::kQualBoss, "a boss room exit did not qualify as a boss");
    m.bossDefeated = false;
    check(story::qualify(m, 0) == 0, "a boss room exit qualified before the boss fell");
    m.event.requester = fpcNm_NPC_GWOLF_e;
    check(story::qualify(m, 1) == 0, "a golden wolf relocation qualified");
    m.event.requester = -1;
    check(story::isSideEffectPoint("D_MN08D", 50, 20), "Zant's arena is no side point");
    m.from = entrance("F_SP108", 0, 0);
    m.to = entrance("R_SP107", 0, 24);
    check((story::qualify(m, 0) & story::kQualSidePoint) != 0, "R_SP107 p24 is no side point");
    m.curated = story::matchCuratedMove(m);
    m.qual = story::qualify(m, 0);
    check(m.strong(), "the capture is not a strong move");

    m.id = "0123456789abcdef";
    m.event.name = "demo";
    m.event.mapToolId = 3;
    m.wolfBefore = false;
    m.wolfAfter = true;
    m.dclAfter = 4;
    const story::MoveRecord back = story::MoveRecord::fromJson(m.toJson());
    check(back.id == m.id && back.to.sameStageRoom("R_SP107", 0) && back.to.point == 24 &&
              back.event.mapToolId == 3 && back.wolfAfter && !back.wolfBefore &&
              back.dclAfter == 4 && back.curated == m.curated && back.qual == m.qual,
        "STORY_MOVE did not survive a JSON round trip");

    // A follower's entrance must be known to the warp table (an unknown point is fatal).
    check(local::isKnownEntrance("R_SP107", 0, 24), "R_SP107 room 0 point 24 unknown");
    check(!local::isKnownEntrance("R_SP107", 0, 99), "R_SP107 room 0 point 99 known");
    check(local::isTeleportableRoom("F_SP108", 0), "F_SP108 room 0 not teleportable");
    check(local::mapName("F_SP108", 0) == "South Faron Woods", "F_SP108 room 0 misnamed");
    for (int i = 0; i < story::kStoryMoveCount; i++) {
        const auto& d = story::kStoryMoves[i];
        if (d.toStage != nullptr && d.toRoom >= 0 && d.followPoint >= 0) {
            check(local::isKnownEntrance(d.toStage, d.toRoom, d.followPoint),
                fmt::format("{} follow point unknown", d.id));
        }
    }

    // Segments and spawn prediction on the live save, restored afterwards.
    dSv_player_status_b_c& b = dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusB();
    const u8 tlv = b.mTransformLevelFlag;
    const u8 dcl = b.mDarkClearLevelFlag;
    const bool f0630 = dComIfGs_isEventBit(dSv_event_flag_c::F_0630);
    const bool m014 = dComIfGs_isEventBit(dSv_event_flag_c::M_014);
    const bool m077 = dComIfGs_isEventBit(dSv_event_flag_c::M_077);
    const bool m071 = dComIfGs_isEventBit(dSv_event_flag_c::M_071);
    const bool f0250 = dComIfGs_isEventBit(dSv_event_flag_c::F_0250);
    const auto restoreBit = [](bool on, u16 bit) {
        if (on) {
            dComIfGs_onEventBit(bit);
        } else {
            dComIfGs_offEventBit(bit);
        }
    };
    {
        // Muted: these writes must not reach the team.
        sync::RemoteApplyGuard guard;
        b.mTransformLevelFlag = 0;
        b.mDarkClearLevelFlag = 0;
        dComIfGs_offEventBit(dSv_event_flag_c::F_0630);
        dComIfGs_offEventBit(dSv_event_flag_c::M_014);
        dComIfGs_offEventBit(dSv_event_flag_c::M_077);
        check(story::activeSegment() == nullptr, "a segment is active on a fresh save");
        check(
            story::predictSpawnWolf(entrance("R_SP107", 0, 24)), "the cell does not spawn a wolf");
        check(!story::predictSpawnWolf(entrance("F_SP108", 0, 0)), "Faron spawns a wolf");
        b.mTransformLevelFlag = 1;
        const story::StorySegment* seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "captured") == 0, "captured is not active");
        check(seg != nullptr && story::segmentAllows(*seg, "R_SP107") &&
                  !story::segmentAllows(*seg, "F_SP108"),
            "captured allows the wrong stages");
        check(seg != nullptr && seg->canonical.valid() &&
                  local::isKnownEntrance(
                      seg->canonical.stage, seg->canonical.room, seg->canonical.point),
            "captured has no loadable entrance");
        dComIfGs_onEventBit(dSv_event_flag_c::M_014);
        seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "ordon-twilight") == 0,
            "ordon-twilight is not active after M_014");
        b.mDarkClearLevelFlag = 1;
        check(story::activeSegment() == nullptr, "a segment is active after Faron is restored");
        check(!story::predictSpawnWolf(entrance("F_SP108", 1, 3)), "the spring spawns a wolf");
        b.mTransformLevelFlag = 0x3;
        seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "eldin-twilight") == 0,
            "eldin-twilight is not active in Eldin's twilight");
        check(seg != nullptr &&
                  story::segmentState(*seg, "F_SP109") == story::SegmentState::Consistent &&
                  story::segmentState(*seg, "F_SP104") == story::SegmentState::Behind &&
                  story::segmentState(*seg, "F_SP115") == story::SegmentState::Inconsistent,
            "eldin-twilight has the wrong core or allowed stages");
        b.mTransformLevelFlag = 0x7;
        b.mDarkClearLevelFlag = 0x3;
        seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "lanayru-twilight") == 0,
            "lanayru-twilight is not active in Lanayru's twilight");
        b.mTransformLevelFlag = 0xF;
        b.mDarkClearLevelFlag = 0x7;
        dComIfGs_onEventBit(dSv_event_flag_c::M_071);
        seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "mdh") == 0, "mdh is not active after M_071");
        check(story::predictSpawnWolf(entrance("F_SP109", 0, 0)), "MDH does not spawn a wolf");
        dComIfGs_onEventBit(dSv_event_flag_c::F_0250);
        seg = story::activeSegment();
        check(seg != nullptr && std::strcmp(seg->id, "wolf-until-sword") == 0,
            "wolf-until-sword is not active after F_0250");
        check(!story::predictSpawnWolf(entrance("F_SP117", 1, 99)),
            "the Master Sword point spawns a wolf");
        dComIfGs_offEventBit(dSv_event_flag_c::M_071);
        dComIfGs_offEventBit(dSv_event_flag_c::F_0250);
        check(story::stageSaveTbl("D_MN05A") == 0x10 && story::stageSaveTbl("F_SP121") == 6 &&
                  story::stageSaveTbl("F_SP1") == -1,
            "stage save tables are wrong");
        b.mTransformLevelFlag = tlv;
        b.mDarkClearLevelFlag = dcl;
        restoreBit(f0630, dSv_event_flag_c::F_0630);
        restoreBit(m014, dSv_event_flag_c::M_014);
        restoreBit(m077, dSv_event_flag_c::M_077);
        restoreBit(m071, dSv_event_flag_c::M_071);
        restoreBit(f0250, dSv_event_flag_c::F_0250);
    }
    return failure;
}

struct ProfileFind {
    int16_t profile;
    fopAc_ac_c* nearest;
    float dist;
};

void* findNearestProfile(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    auto* find = static_cast<ProfileFind*>(data);
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (fopAcM_GetName(actor) != find->profile || player == nullptr) {
        return nullptr;
    }
    const float d = actor->current.pos.abs(player->current.pos);
    if (find->nearest == nullptr || d < find->dist) {
        find->nearest = actor;
        find->dist = d;
    }
    return nullptr;
}

void movePlayerTo(const cXyz& pos) {
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    cXyz at = pos;
    player->setPlayerPosAndAngle(&at, player->shape_angle.y, TRUE);
    interp::requestPresentationSync();
}

// A defeated boss's warp hole plays its own BOSS_WARPIN (the WARP_CHECK choice skipped).
std::optional<bool> triggerBossWarp(StepContext& ctx) {
    auto* warp = static_cast<daObjBossWarp_c*>(fopAcM_SearchByName(fpcNm_Obj_BossWarp_e));
    if (warp == nullptr || warp->mAction != daObjBossWarp_c::ACT_WAIT_WARP ||
        dComIfGp_event_runCheck())
    {
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("triggerStory bossWarp: no waiting warp hole (found {}, action {})",
                warp != nullptr, warp != nullptr ? warp->mAction : -1));
        }
        return false;
    }
    movePlayerTo(warp->current.pos);
    warp->setAction(daObjBossWarp_c::ACT_ORDER_WARP_EVENT);
    fopAcM_orderOtherEventId(
        warp, warp->mBossWarpInEventId, warp->mBossWarpInMapToolId, 0xFFFF, 0, 1);
    warp->eventInfo.onCondition(2);
    TwiliLog.info("[autotest] triggerStory: boss warp in {} ordered BOSS_WARPIN (scene list {})",
        currentStage(), warp->getSceneListNo());
    return true;
}

std::optional<bool> triggerStory(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string via = step.value("via", std::string("requester"));
    if (via == "bossWarp") {
        return triggerBossWarp(ctx);
    }
    if (ctx.begun) {
        return true;
    }
    // The area's tears complete: a reload runs kytag04's own warp to the spring.
    if (via == "tearsFull") {
        const int area = step.value("area", static_cast<int>(dComIfGp_getStartStageDarkArea()));
        const int num = step.value("num", 16);
        dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(num));
        dComIfGs_setRestartRoomParam(0);
        dComIfGp_setNextStage(currentStage(), dComIfGp_getStartStagePoint(),
            static_cast<s8>(dComIfGp_roomControl_getStayNo()), -1, 0.0f, 0, 1, 0, 0, 1, 0);
        TwiliLog.info("[autotest] triggerStory: area {} tears set to {}, reloading {}", area, num,
            currentStage());
        return true;
    }
    // Midna requests a potential event; once it runs the stage loads (tickForcedMove).
    if (via == "forcedMove") {
        fopAc_ac_c* midna = daPy_py_c::getMidnaActor();
        if (midna == nullptr) {
            ctx.fail("triggerStory forcedMove: no Midna");
            return false;
        }
        fopAcM_orderPotentialEvent(midna, 0, 0xFFFF, 0);
        sForcedMove = step;
        TwiliLog.info("[autotest] triggerStory: Midna ordered a potential event");
        return true;
    }
    // That NPC orders table entry `index` through its own evtOrder, as its action code would.
    if (via == "npc") {
        ProfileFind find{static_cast<int16_t>(step.value("profile", -1)), nullptr, 0.0f};
        fopAcM_Search(&findNearestProfile, &find);
        auto* npc = static_cast<daNpcT_c*>(find.nearest);
        const int index = step.value("index", 0);
        if (npc == nullptr || !story::npc::isNpcT(find.profile) ||
            story::npc::eventName(npc, index).empty())
        {
            ctx.fail(fmt::format("triggerStory npc: no NPC {} with entry {}", find.profile, index));
            return false;
        }
        story::npc::placeOrder(npc, index);
        TwiliLog.info("[autotest] triggerStory: NPC {} orders entry {} '{}'", find.profile, index,
            story::npc::eventName(npc, index));
        return true;
    }
    if (via == "actor") {
        ProfileFind find{static_cast<int16_t>(step.value("profile", -1)), nullptr, 0.0f};
        fopAcM_Search(&findNearestProfile, &find);
        if (find.nearest == nullptr) {
            ctx.fail(fmt::format("triggerStory actor: no actor of profile {}", find.profile));
            return false;
        }
        movePlayerTo(find.nearest->current.pos);
        TwiliLog.info("[autotest] triggerStory: moved onto profile {} ({:.0f} away)", find.profile,
            find.dist);
        return true;
    }
    // Arrive at `point` / `layer`, whose PLYR entry starts the map event.
    if (via == "arrival") {
        const int point = step.value("point", 0);
        const int layer = step.value("layer", -1);
        dComIfGs_setRestartRoomParam(0);
        dComIfGp_setNextStage(currentStage(), static_cast<s16>(point),
            static_cast<s8>(step.value("room", static_cast<int>(dComIfGp_roomControl_getStayNo()))),
            static_cast<s8>(layer), 0.0f, 0, 1, 0, 0, 1, 0);
        TwiliLog.info("[autotest] triggerStory: arriving in {} at point {} layer {}",
            currentStage(), point, layer);
        return true;
    }
    TagFind find{static_cast<uint8_t>(step.value("mapToolId", 0))};
    auto* tag = static_cast<daTag_Event_c*>(fopAcM_Search(&findTag, &find));
    if (tag == nullptr) {
        ctx.fail(fmt::format("triggerStory: no event tag for map event {}", find.eventNo));
        return false;
    }
    if (via == "walk") {
        daPy_py_c* player = dComIfGp_getLinkPlayer();
        cXyz pos = tag->current.pos;
        player->setPlayerPosAndAngle(&pos, player->shape_angle.y, TRUE);
        interp::requestPresentationSync();
        TwiliLog.info(
            "[autotest] triggerStory: moved into the tag area of map event {}", find.eventNo);
        return true;
    }
    // What actionHunt does once Link stands in the area.
    tag->setActio(daTag_Event_c::ACTION_READY);
    fopAcM_orderOtherEventId(tag, tag->mEventIdx, tag->getEventNo(), 0xFFFF, 0, 1);
    TwiliLog.info("[autotest] triggerStory: tag ordered map event {} (event {})", find.eventNo,
        tag->mEventIdx);
    return true;
}

std::vector<daObjDrop_c*> s_tears;

void* collectTears(void* proc, void*) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (fopAcM_GetName(actor) == fpcNm_Obj_Drop_e) {
        s_tears.push_back(static_cast<daObjDrop_c*>(actor));
    }
    return nullptr;
}

// The live tears of the stage, by TBOX number.
const std::vector<daObjDrop_c*>& liveTears() {
    s_tears.clear();
    fopAcM_Search(&collectTears, nullptr);
    std::sort(s_tears.begin(), s_tears.end(),
        [](daObjDrop_c* a, daObjDrop_c* b) { return a->getSave() < b->getSave(); });
    return s_tears;
}

std::string tearList() {
    std::string s;
    for (daObjDrop_c* t : liveTears()) {
        s += fmt::format("{}{}(mode {})", s.empty() ? "" : " ", t->getSave(), t->mMode);
    }
    return s;
}

// Frees the nth tear from its insect and picks it up (dropGet; its draw-in runs only on screen).
std::optional<bool> collectTear(StepContext& ctx) {
    const int area = dComIfGp_getStartStageDarkArea();
    if (!ctx.begun) {
        const auto& tears = liveTears();
        const size_t nth = static_cast<size_t>(ctx.step.value("nth", 0));
        if (nth >= tears.size()) {
            ctx.fail(fmt::format("collectTear: no tear {} (live: {})", nth, tearList()));
            return false;
        }
        daObjDrop_c* tear = tears[nth];
        sTearTbox = tear->getSave();
        sTearPicked = false;
        sTearCountBefore = dComIfGs_getLightDropNum(static_cast<u8>(area));
        // A tear still carried by its shadow insect appears once the insect is gone.
        fopAc_ac_c* insect = tear->mMode == daObjDrop_c::MODE_PARENT_WAIT_e ?
                                 fopAcM_SearchByID(tear->parentActorID) :
                                 nullptr;
        if (insect != nullptr &&
            (fopAcM_GetName(insect) == fpcNm_E_YM_e || fopAcM_GetName(insect) == fpcNm_E_YMB_e))
        {
            fopAcM_delete(insect);
        }
        TwiliLog.info("[autotest] collectTear: tear {} of area {} (count {}; live: {})", sTearTbox,
            area, sTearCountBefore, tearList());
        return false;
    }
    daObjDrop_c* tear = nullptr;
    for (daObjDrop_c* t : liveTears()) {
        if (t->getSave() == sTearTbox) {
            tear = t;
        }
    }
    if (tear == nullptr) {
        TwiliLog.info("[autotest] tear {} collected: count {} -> {}", sTearTbox, sTearCountBefore,
            dComIfGs_getLightDropNum(static_cast<u8>(area)));
        return true;
    }
    if (tear->mMode == daObjDrop_c::MODE_WAIT_e && !sTearPicked) {
        sTearPicked = true;
        tear->mSetCollectDrop = true;
        tear->dropGet();
        fopAcM_delete(tear);
        TwiliLog.info("[autotest] tear {} picked up", sTearTbox);
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        ctx.fail(fmt::format("collectTear: tear {} still there (mode {} action {})", sTearTbox,
            tear->mMode, tear->mModeAction));
    }
    return false;
}

struct SweepEntry {
    story::Entrance e;
    std::string what;
};
std::vector<SweepEntry> sSweep;
size_t sSweepIndex = 0;
int sSweepPhase = 0;  // 0 idle wait, 1 loading
bool sSweepPredictWolf = false;
int sSweepQuiet = 0;
bool sSweepChecked = false;
std::string sSweepFailures;

void addSweep(const story::Entrance& e, const std::string& what) {
    if (!e.valid()) {
        return;
    }
    for (const SweepEntry& s : sSweep) {
        if (s.e.sameStageRoom(e.stage, e.room) && s.e.point == e.point) {
            return;
        }
    }
    sSweep.push_back({e, what});
}

// Loads every segment entrance, follow point and learned destination: known, predicted form.
std::optional<bool> entranceSweep(StepContext& ctx) {
    if (!ctx.begun) {
        sSweep.clear();
        sSweepIndex = 0;
        sSweepPhase = 0;
        sSweepQuiet = 0;
        sSweepFailures.clear();
        for (const char* id : {"captured", "ordon-twilight", "mdh", "wolf-until-sword",
                 "lanayru-twilight", "eldin-twilight"})
        {
            if (const story::StorySegment* seg = story::segmentById(id)) {
                addSweep(story::segmentEntrance(*seg), std::string("segment ") + id);
            }
        }
        for (int i = 0; i < story::kStoryMoveCount; i++) {
            const story::StoryMoveDef& d = story::kStoryMoves[i];
            if (d.toStage != nullptr && d.toRoom >= 0 && d.followPoint >= 0) {
                addSweep(entrance(d.toStage, d.toRoom, d.followPoint), std::string("row ") + d.id);
            }
        }
        for (const auto& [key, e] : story::storylog::learned()) {
            addSweep(e.move.to, "learned " + key);
        }
        TwiliLog.info("[autotest] entrance sweep: {} entrance(s)", sSweep.size());
    }
    const bool idle = !dComIfGp_isEnableNextStage() && !dComIfGp_event_runCheck() &&
                      local::liveLink() != nullptr;
    if (sSweepPhase == 1) {
        sSweepQuiet = idle ? sSweepQuiet + 1 : 0;
        const SweepEntry& s = sSweep[sSweepIndex];
        // The spawn form, before an arrival scene changes it.
        if (!sSweepChecked && local::liveLink() != nullptr && !dComIfGp_isEnableNextStage() &&
            std::strncmp(currentStage(), s.e.stage, 8) == 0)
        {
            sSweepChecked = true;
            const bool wolf = local::liveLink()->checkWolf() != 0;
            TwiliLog.info("[autotest] sweep {} room {} point {} ({}): layer {} {} (predicted {})",
                s.e.stage, s.e.room, s.e.point, s.what, dComIfG_play_c::getLayerNo(0),
                wolf ? "wolf" : "human", sSweepPredictWolf ? "wolf" : "human");
            if (wolf != sSweepPredictWolf) {
                sSweepFailures += fmt::format(" {}/{}/{} ({}) spawned a {};", s.e.stage, s.e.room,
                    s.e.point, s.what, wolf ? "wolf" : "human");
            }
        }
        if ((sSweepChecked && sSweepQuiet >= 45) || ctx.ticks % 2700 == 2699) {
            sSweepPhase = 0;
            sSweepIndex++;
            sSweepQuiet = 0;
        } else {
            if (dComIfGp_event_runCheck() && !padBusy() && ctx.ticks % 20 == 0) {
                pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
            }
            return false;
        }
    }
    if (sSweepIndex >= sSweep.size()) {
        if (!sSweepFailures.empty()) {
            ctx.fail("entranceSweep:" + sSweepFailures);
            return false;
        }
        TwiliLog.info("[autotest] entrance sweep passed");
        return true;
    }
    if (!idle) {
        return false;
    }
    const SweepEntry& s = sSweep[sSweepIndex];
    if (!local::isKnownEntrance(s.e.stage, s.e.room, s.e.point)) {
        sSweepFailures += fmt::format(" {}/{}/{} ({}) unknown;", s.e.stage, s.e.room, s.e.point,
            s.what);
        sSweepIndex++;
        return false;
    }
    sSweepPredictWolf = story::predictSpawnWolf(s.e);
    dComIfGs_setRestartRoomParam(0);
    dComIfGp_setNextStage(s.e.stage, s.e.point, s.e.room, -1, 0.0f, 0, 1, 0, 0, 1, 0);
    sSweepPhase = 1;
    sSweepQuiet = 0;
    sSweepChecked = false;
    return false;
}

std::optional<bool> expectStoryMove(StepContext& ctx) {
    const json& step = ctx.step;
    const sd::State& st = sd::state();
    const std::string role = step.value("role", std::string("sent"));
    // "none": we sent none; "notReceived": none reached us.
    if (role == "none" || role == "notReceived") {
        const uint32_t count = role == "none" ? st.movesSent : st.movesReceived;
        if (count != 0) {
            ctx.fail(
                fmt::format("expectStoryMove {}: {} move(s) ({})", role, count, sd::debugText()));
            return false;
        }
        return ctx.seconds >= step.value("sec", 0.0);
    }
    const uint32_t count = role == "sent" ? st.movesSent : st.movesReceived;
    const std::string curated = step.value("curated", std::string{});
    const uint32_t qualHas = step.value("qualHas", 0u);
    const bool wantCached = step.value("cached", false);
    const story::MoveRecord& m = st.lastMove;
    const bool placeOk =
        (!step.contains("toStage") ||
            std::strncmp(m.to.stage, step.value("toStage", std::string{}).c_str(), 8) == 0) &&
        (!step.contains("toRoom") || m.to.room == step.value("toRoom", -1)) &&
        (!step.contains("toPoint") || m.to.point == step.value("toPoint", -1));
    if (count > 0 && (curated.empty() || st.lastMoveCurated == curated) &&
        (st.lastMoveQual & qualHas) == qualHas && (!wantCached || st.lastMoveFromCache) && placeOk)
    {
        TwiliLog.info("[autotest] story move {} (curated '{}', qual 0x{:X}) to {} room {} point {} "
                      "layer {} key '{}'",
            role, st.lastMoveCurated, st.lastMoveQual, m.to.stage, m.to.room, m.to.point,
            m.to.layer, m.key);
        return true;
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        ctx.fail(fmt::format("expectStoryMove {}: {} move(s), last curated '{}' qual 0x{:X} "
                             "cached {} to {} room {} point {} ({})",
            role, count, st.lastMoveCurated, st.lastMoveQual, st.lastMoveFromCache, m.to.stage,
            m.to.room, m.to.point, sd::debugText()));
    }
    return false;
}

std::optional<bool> catchUp(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string want = step.value("expectKind", std::string("entrance"));
    const story::CatchUpPlan& plan = sd::state().plan;
    const std::string have = story::catchUpKindName(plan.kind);
    // toStage: the plan must lead there.
    const std::string toStage = step.value("toStage", std::string{});
    if (have != want ||
        (!toStage.empty() && std::strncmp(plan.entrance.stage, toStage.c_str(), 8) != 0))
    {
        if (ctx.seconds < ctx.timeout(10.0)) {
            return false;  // a merge may still be coming
        }
        ctx.fail(fmt::format("catchUp: plan {} '{}' to {} - {} (want {} {})", have, plan.title,
            plan.entrance.stage, plan.reason, want, toStage));
        return false;
    }
    if (plan.kind == story::CatchUpPlan::Kind::None || !step.value("start", true)) {
        TwiliLog.info("[autotest] catch-up plan {}: {}", have, plan.reason);
        return true;
    }
    if (const char* why = sd::catchUpBlockCode()) {
        if (ctx.seconds < ctx.timeout(30.0)) {
            return false;
        }
        ctx.fail(fmt::format("catchUp: blocked ({})", why));
        return false;
    }
    TwiliLog.info("[autotest] catch up: {} ({})", plan.title, plan.reason);
    if (!sd::startCatchUpPlan()) {
        ctx.fail("catchUp: the plan did not start");
        return false;
    }
    return true;
}

// The capture ends at d_GameOver's "Save?" menu: cancel it, or pick "No" (stick down + A).
std::optional<bool> dismissSaveRequest(StepContext& ctx) {
    const story::Instance* in = story::tracker().lastInstance();
    const bool saveReq =
        dComIfGp_event_runCheck() && in != nullptr && in->running() && in->name == "SAVEREQ";
    if (!ctx.begun) {
        sSaveReqSeen = false;
        sSavePulse = 0;
        sSavePulseTick = 0;
    }
    if (saveReq) {
        sSaveReqSeen = true;
        if (!padBusy() && ctx.ticks - sSavePulseTick >= 25) {
            sSavePulseTick = ctx.ticks;
            switch (sSavePulse++ % 3) {
            case 0:
                pulsePad(0.0f, 0.0f, PAD_BUTTON_B, 3);
                break;
            case 1:
                pulsePad(0.0f, -1.0f, 0, 3);
                break;
            default:
                pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
                break;
            }
        }
        return false;
    }
    if (sSaveReqSeen) {
        TwiliLog.info("[autotest] save request dismissed after {} press(es)", sSavePulse);
        return true;
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        if (ctx.step.value("optional", false)) {
            return true;
        }
        ctx.fail("dismissSaveRequest: no save request came up");
    }
    return false;
}

// The stage change of a forcedMove, once its event runs.
void tickForcedMove() {
    if (sForcedMove.is_null()) {
        return;
    }
    fopAc_ac_c* midna = daPy_py_c::getMidnaActor();
    if (!dComIfGp_event_runCheck()) {
        // Refused while another event ran (a room's start event): order it again.
        if (midna != nullptr && ++sForcedRetry % 10 == 0) {
            fopAcM_orderPotentialEvent(midna, 0, 0xFFFF, 0);
        }
        return;
    }
    if (dComIfGp_getEvent()->getPt1() != midna) {
        return;
    }
    const json step = std::move(sForcedMove);
    sForcedMove = json();
    if (step.contains("bit")) {
        dComIfGs_onEventBit(static_cast<u16>(step.value("bit", 0)));
    }
    const std::string stage = step.value("stage", std::string(currentStage()));
    dComIfGs_setRestartRoomParam(0);
    dComIfGp_setNextStage(stage.c_str(), static_cast<s16>(step.value("point", 0)),
        static_cast<s8>(step.value("room", 0)), static_cast<s8>(step.value("layer", -1)), 0.0f, 0,
        1, 0, 0, 1, 0);
    TwiliLog.info("[autotest] forced move to {} room {} point {} layer {}", stage,
        step.value("room", 0), step.value("point", 0), step.value("layer", -1));
}

std::optional<bool> storySteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;
    sd::State& st = sd::state();
    tickForcedMove();

    if (op == "storySelfTest") {
        const std::string failure = selfTest();
        if (!failure.empty()) {
            ctx.fail("storySelfTest: " + failure);
            return false;
        }
        TwiliLog.info("[autotest] story self-test passed");
        return true;
    }

    if (op == "dumpStory") {
        TwiliLog.info("[autotest] story: {}", sd::debugText());
        story::dumpStageEvents();
        return true;
    }

    if (op == "expectLayer") {
        const int have = dComIfG_play_c::getLayerNo(0);
        const int want = step.value("natural", false) ?
                             dComIfG_play_c::getLayerNo_common(
                                 currentStage(), dComIfGp_roomControl_getStayNo(), -1) :
                             step.value("layer", -1);
        if (have != want) {
            ctx.fail(fmt::format("expectLayer: layer {} (want {})", have, want));
            return false;
        }
        TwiliLog.info("[autotest] layer {}", have);
        return true;
    }

    if (op == "triggerStory") {
        return triggerStory(ctx);
    }

    if (op == "expectStoryMove") {
        return expectStoryMove(ctx);
    }

    if (op == "expectPrompt") {
        const std::string kind = step.value("kind", std::string("move"));
        // titleHas: text the move prompt's title must contain.
        const std::string title =
            st.team.valid ? sd::moveTitle(st.team.move) : std::string{};
        const std::string want = step.value("titleHas", std::string{});
        if (st.prompt.showing && kind == story::promptKindName(st.prompt.kind) &&
            (want.empty() || title.find(want) != std::string::npos))
        {
            TwiliLog.info("[autotest] story prompt {} is showing: '{}'", kind, title);
            return true;
        }
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail(fmt::format("expectPrompt {}: prompt {} showing {} title '{}' ({})", kind,
                story::promptKindName(st.prompt.kind), st.prompt.showing, title, sd::debugText()));
        }
        return false;
    }

    if (op == "expectNoPrompt") {
        if (st.prompt.kind != story::PromptKind::None) {
            ctx.fail(fmt::format("expectNoPrompt: prompt {} offered ({})",
                story::promptKindName(st.prompt.kind), sd::debugText()));
            return false;
        }
        return ctx.seconds >= step.value("sec", 5.0);
    }

    if (op == "answerPrompt") {
        const std::string answer = step.value("answer", std::string("follow"));
        if (st.prompt.kind == story::PromptKind::None) {
            ctx.fail("answerPrompt: no story prompt");
            return false;
        }
        sd::answerPrompt(answer == "decline" ? story::PromptAnswer::Decline :
                         answer == "catchup" ? story::PromptAnswer::CatchUp :
                                               story::PromptAnswer::Follow);
        return true;
    }

    if (op == "catchUp") {
        return catchUp(ctx);
    }

    if (op == "expectStoryLoad") {
        const json want = step.value("state", json("arrived"));
        const std::string have = story::loadPhaseName(st.load.phase);
        const std::string reason = step.value("reason", std::string{});
        if (step.contains("layerArg") && st.load.phase != story::LoadPhase::Idle &&
            st.load.layerArg != step.value("layerArg", -1))
        {
            ctx.fail(fmt::format("expectStoryLoad: layer arg {} (want {})", st.load.layerArg,
                step.value("layerArg", -1)));
            return false;
        }
        if (matchesAny(want, have) && (reason.empty() || reason == st.load.reason)) {
            return true;
        }
        const bool final =
            st.load.phase == story::LoadPhase::Arrived || st.load.phase == story::LoadPhase::Failed;
        if (final && !matchesAny(want, have)) {
            ctx.fail(fmt::format("expectStoryLoad: {} '{}'", have, st.load.reason));
            return false;
        }
        if (ctx.seconds > ctx.timeout(90.0)) {
            ctx.fail(fmt::format("expectStoryLoad: still {} '{}'", have, st.load.reason));
        }
        return false;
    }

    if (op == "expectConsistent") {
        const bool want = step.value("value", true);
        const std::string seg = step.value("segment", std::string{});
        if (st.plan.inconsistent == want || (!seg.empty() && st.plan.segment != seg)) {
            if (ctx.seconds < ctx.timeout(10.0)) {
                return false;
            }
            ctx.fail(fmt::format("expectConsistent: inconsistent {} segment '{}' in {} ({})",
                st.plan.inconsistent, st.plan.segment, currentStage(), st.plan.reason));
            return false;
        }
        return true;
    }

    if (op == "expectJoin") {
        const json want = step.value("state", json("running"));
        const std::string have = story::joinStateName(st.join.state);
        const std::string reason = step.value("reason", std::string{});
        const bool matches = matchesAny(want, have) && (reason.empty() || reason == st.join.reason);
        // holdSec: the state must hold that long.
        if (step.contains("holdSec")) {
            if (!matches) {
                ctx.fail(fmt::format(
                    "expectJoin: became {} '{}' ({})", have, st.join.reason, sd::debugText()));
                return false;
            }
            return ctx.seconds >= step.value("holdSec", 0.0);
        }
        if (matches) {
            TwiliLog.info("[autotest] story join {} {}", have, st.join.reason);
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(
                fmt::format("expectJoin: {} '{}' ({})", have, st.join.reason, sd::debugText()));
        }
        return false;
    }

    if (op == "waitStorySettled") {
        const bool settled = !dComIfGp_isEnableNextStage() && !dComIfGp_event_runCheck() &&
                             !story::tracker().movePending() && !story::loadActive() &&
                             !sd::followChainOpen() && story::tracker().quietTicks() >= 60 &&
                             sForcedMove.is_null();
        // pressA: advances the text of the scenes on the way.
        if (!settled && step.value("pressA", false) && dComIfGp_event_runCheck() && !padBusy() &&
            ctx.ticks % 20 == 0)
        {
            pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
        }
        if (settled) {
            TwiliLog.info("[autotest] story settled in {} room {} layer {}", currentStage(),
                dComIfGp_roomControl_getStayNo(), dComIfG_play_c::getLayerNo(0));
            return true;
        }
        if (ctx.seconds > ctx.timeout(180.0)) {
            ctx.fail(fmt::format("waitStorySettled: {}", sd::debugText()));
        }
        return false;
    }

    if (op == "forcePullInBlocker") {
        const json code = step.value("code", json());
        st.forcedBlocker = code.is_string() ? code.get<std::string>() : std::string{};
        return true;
    }

    // Level bits travel in world state only.
    if (op == "setTransformLevel" || op == "setDarkClear") {
        const int level = step.value("level", 0);
        const bool set = step.value("set", true);
        if (op == "setTransformLevel") {
            set ? dComIfGs_onTransformLV(level) : dComIfGs_offTransformLV(level);
        } else {
            set ? dComIfGs_onDarkClearLV(level) : dComIfGs_offDarkClearLV(level);
        }
        return true;
    }

    if (op == "expectTransformLevel" || op == "expectDarkClear") {
        const int level = step.value("level", 0);
        const bool want = step.value("set", true);
        const bool have = op == "expectTransformLevel" ? dComIfGs_isTransformLV(level) != FALSE :
                                                         dComIfGs_isDarkClearLV(level) != FALSE;
        if (have == want) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("{} {} never became {}", op, level, want));
        }
        return false;
    }

    if (op == "dismissSaveRequest") {
        return dismissSaveRequest(ctx);
    }

    // The whole save plus where we stand, for a scenario's start.fixture.
    if (op == "saveFixture") {
        namespace fs = std::filesystem;
        const std::string name = step.value("name", std::string("fixture"));
        const fs::path dir = fs::path(detail::state().resultPath).parent_path() / "fixtures";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto* bytes = reinterpret_cast<const uint8_t*>(dComIfGs_getSaveData());
        const daPy_py_c* player = dComIfGp_getLinkPlayer();
        const json fixture = {
            {"stage", currentStage()},
            {"room", dComIfGp_roomControl_getStayNo()},
            {"point", dComIfGp_getStartStagePoint()},
            {"layer", dComIfG_play_c::getLayerNo(0)},
            {"pos", player != nullptr ? json::array({player->current.pos.x, player->current.pos.y,
                                            player->current.pos.z}) :
                                        json::array()},
            {"save", base64::encode({bytes, sizeof(dSv_save_c)})},
        };
        std::ofstream out(dir / (name + ".json"), std::ios::trunc);
        out << fixture.dump(1);
        if (!out) {
            ctx.fail("saveFixture: cannot write " + (dir / (name + ".json")).string());
            return false;
        }
        TwiliLog.info("[autotest] fixture '{}' saved in {}", name, dir.string());
        return true;
    }

    if (op == "expectSegment") {
        const std::string id = step.value("id", std::string{});
        const std::string want = step.value("state", std::string("consistent"));
        const story::StorySegment* seg = story::activeSegment();
        const std::string haveId = seg != nullptr ? seg->id : "none";
        const std::string have =
            seg != nullptr ? story::segmentStateName(story::segmentState(*seg, currentStage())) :
                             "none";
        if ((id.empty() || id == haveId) && (haveId == "none" || have == want)) {
            TwiliLog.info("[autotest] segment {} {} in {}", haveId, have, currentStage());
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectSegment: {} {} in {} (want {} {})", haveId, have,
                currentStage(), id, want));
        }
        return false;
    }

    // key or curated; count (default 1): how many learned moves match.
    if (op == "expectLearned") {
        const std::string key = step.value("key", std::string{});
        const std::string curated = step.value("curated", std::string{});
        int found = 0;
        for (const auto& [k, e] : story::storylog::learned()) {
            const story::StoryMoveDef* def = story::curatedMove(e.move.curated);
            if ((!key.empty() && k == key) ||
                (!curated.empty() && def != nullptr && curated == def->id))
            {
                found++;
            }
        }
        const int want = step.value("count", 1);
        if (key.empty() && curated.empty() ? static_cast<int>(story::storylog::learnedCount()) >= want :
                                               found == want)
        {
            TwiliLog.info("[autotest] learned: {} move(s), {} matching", story::storylog::learnedCount(),
                found);
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectLearned: {} matching of {} (want {})", found,
                story::storylog::learnedCount(), want));
        }
        return false;
    }

    // from {stage, room?, point?} -> to {stage, room, point, layer?}, like a shuffled entrance.
    if (op == "testRemap") {
        sRemap = step.contains("from") ? json{{"from", step["from"]}, {"to", step["to"]}} : json();
        return true;
    }

    // Logs the daNpcT_c actors of the stage (for picking a pull-in test NPC).
    if (op == "listNpcs") {
        s_tears.clear();
        fopAcM_Search(
            [](void* proc, void*) -> void* {
                auto* a = static_cast<fopAc_ac_c*>(proc);
                if (story::npc::isNpcT(fopAcM_GetName(a))) {
                    TwiliLog.info("[autotest] npc 0x{:X} room {} at ({:.0f} {:.0f} {:.0f})",
                        fopAcM_GetName(a), fopAcM_GetRoomNo(a), a->current.pos.x, a->current.pos.y,
                        a->current.pos.z);
                }
                return nullptr;
            },
            nullptr);
        return true;
    }

    if (op == "allowNpcEvent") {
        story::npc::allowForTest(
            static_cast<int16_t>(step.value("profile", -1)), step.value("event", std::string{}));
        return true;
    }

    if (op == "perturbStoryDigest") {
        story::npc::perturbDigestForTest(step.value("value", true));
        return true;
    }

    if (op == "forgetLearned") {
        story::storylog::forget();
        return true;
    }

    if (op == "entranceSweep") {
        return entranceSweep(ctx);
    }

    if (op == "expectTransient") {
        const bool want = step.value("value", true);
        if (st.team.transient == want) {
            TwiliLog.info("[autotest] team move transient {}", want);
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("expectTransient: {} ({})", st.team.transient, sd::debugText()));
        }
        return false;
    }

    if (op == "collectTear") {
        return collectTear(ctx);
    }

    // countTears marks the live tear count; expectTears waits for mark + delta.
    if (op == "countTears") {
        sTearsAtMark = static_cast<int>(liveTears().size());
        TwiliLog.info("[autotest] {} live tear(s): {}", sTearsAtMark, tearList());
        return true;
    }
    if (op == "expectTears") {
        const int want = sTearsAtMark + step.value("delta", 0);
        const int have = static_cast<int>(liveTears().size());
        if (have == want) {
            TwiliLog.info("[autotest] {} live tear(s): {}", have, tearList());
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("expectTears: {} live (want {}): {}", have, want, tearList()));
        }
        return false;
    }
    // Received tears stay on screen, so a pickup can race a teammate's.
    if (op == "keepTakenTears") {
        sync::keepTakenTearsForTest(step.value("value", true));
        return true;
    }

    if (op == "expectLightDrops") {
        const int area = step.value("area", 0);
        const int want = step.value("num", 0);
        const int have = dComIfGs_getLightDropNum(static_cast<u8>(area));
        if (have == want) {
            TwiliLog.info("[autotest] area {} has {} tear(s)", area, have);
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("expectLightDrops: area {} has {} (want {})", area, have, want));
        }
        return false;
    }

    if (op == "storyPrompts") {
        config::setBool(config::Var::StoryPrompts, step.value("value", true));
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&storySteps);

}  // namespace
}  // namespace twili::autotest
