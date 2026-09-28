// Steps for story sync; reference in the runner README.

#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Host.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "story/StoryState.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "dolphin/pad.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace sd = story::detail;

bool sSaveReqSeen = false;
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
    const int light = story::matchCuratedMove(m);
    check(light >= 0 && std::strcmp(story::kStoryMoves[light].id, "faron-light") == 0,
        "-> F_SP108 room 1 point 3 is not faron-light");
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
        b.mTransformLevelFlag = tlv;
        b.mDarkClearLevelFlag = dcl;
        restoreBit(f0630, dSv_event_flag_c::F_0630);
        restoreBit(m014, dSv_event_flag_c::M_014);
        restoreBit(m077, dSv_event_flag_c::M_077);
    }
    return failure;
}

std::optional<bool> triggerStory(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string via = step.value("via", std::string("requester"));
    if (ctx.begun) {
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
    if (count > 0 && (curated.empty() || st.lastMoveCurated == curated) &&
        (st.lastMoveQual & qualHas) == qualHas && (!wantCached || st.lastMoveFromCache))
    {
        TwiliLog.info("[autotest] story move {} (curated '{}', qual 0x{:X})", role,
            st.lastMoveCurated, st.lastMoveQual);
        return true;
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        ctx.fail(fmt::format("expectStoryMove {}: {} move(s), last curated '{}' qual 0x{:X} "
                             "cached {} ({})",
            role, count, st.lastMoveCurated, st.lastMoveQual, st.lastMoveFromCache,
            sd::debugText()));
    }
    return false;
}

std::optional<bool> catchUp(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string want = step.value("expectKind", std::string("entrance"));
    const story::CatchUpPlan& plan = sd::state().plan;
    const std::string have = story::catchUpKindName(plan.kind);
    if (have != want) {
        if (ctx.seconds < ctx.timeout(10.0)) {
            return false;  // a merge may still be coming
        }
        ctx.fail(fmt::format(
            "catchUp: plan {} '{}' - {} (want {})", have, plan.title, plan.reason, want));
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

// The capture chain stops in the cell at SAVEREQ, d_GameOver's "Save?" menu: cancel it, or pick
// "No" with stick down + A if cancelling is not offered.
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

std::optional<bool> storySteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;
    sd::State& st = sd::state();

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
        if (st.prompt.showing && kind == story::promptKindName(st.prompt.kind)) {
            TwiliLog.info("[autotest] story prompt {} is showing", kind);
            return true;
        }
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail(fmt::format("expectPrompt {}: prompt {} showing {} ({})", kind,
                story::promptKindName(st.prompt.kind), st.prompt.showing, sd::debugText()));
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
                             !sd::followChainOpen() && story::tracker().quietTicks() >= 60;
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

    if (op == "expectTransformLevel") {
        const int level = step.value("level", 0);
        const bool want = step.value("set", true);
        if ((dComIfGs_isTransformLV(level) != FALSE) == want) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("expectTransformLevel {} never became {}", level, want));
        }
        return false;
    }

    if (op == "dismissSaveRequest") {
        return dismissSaveRequest(ctx);
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
