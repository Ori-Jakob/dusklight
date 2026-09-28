// Steps for PvP (pvp/, the dummy's hurtbox in actors/DummyPlayer.cpp). Life values are quarter
// hearts: the saved life plus the change the meter has not applied yet.
//
// setLife  value (12)
// markLife
//     Remembers our life for expectLifeDelta.
// expectLife  value | min | max, timeoutSec (10)
// expectLifeDelta  delta, frames (60), timeoutSec (10)
//     Until life - marked == delta; fails at once past it. delta 0 must hold for `frames` ticks.
// approachDummy  dist (110)
//     Our Link `dist` in front of the first peer's dummy, facing it, held there for 10 ticks.
// sendPvpHit  target, kind ("sword"), damage (2), knockback ("light"), dirY, blocked (false)
//     A real DAMAGE_PLAYER without our collision pass or cooldown; viewSeq from our dummy of it.
// expectPvpResult  target, result (string or list), reason, damage, timeoutSec (10)
//     The DAMAGE_RESULT of the last hit we sent to `target`.
// expectPvpStats  target, sent, applied, blocked, dropped, refused, damage, timeoutSec (0)
//     Our counters for hits on `target`; only the fields given are checked.
// expectPvpTaken  count (applied + blocked), blocked, dropped, damage, reason, timeoutSec (0)
// expectDummyHurtbox  registered (true), guard, timeoutSec (0)
//     Whether the first peer's dummy registered its PvP hurtbox this tick (in shield mode).
// expectReaction  knockback ("light", "knockdown" or "none"), timeoutSec (10)
//     Until our Link plays that reaction (PROC_DAMAGE / PROC_WOLF_DAMAGE, or
//     checkCameraLargeDamage); "none": neither, and the i-frames are over.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "pvp/Pvp.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace twili::autotest {
namespace {

using nlohmann::json;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

fopAc_ac_c* firstPeerDummy() {
    auto& b = Session::instance();
    for (const auto& [id, c] : b.clients()) {
        if (peerInMyLayer(c)) {
            if (fopAc_ac_c* dummy = b.dummyActorForClient(id)) {
                return dummy;
            }
        }
    }
    return nullptr;
}

const Client* findPeer(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.online && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

int currentLife() {
    return dComIfGs_getLife() + static_cast<int>(dComIfGp_getItemLifeCount());
}

int sMarkedLife = 0;

// The first field of `step` named in `fields` that differs, or "".
std::string countMismatch(
    const json& step, std::initializer_list<std::pair<const char*, int64_t>> fields) {
    for (const auto& [key, have] : fields) {
        if (step.contains(key) && step[key].get<int64_t>() != have) {
            return fmt::format("{} is {}, want {}", key, have, step[key].get<int64_t>());
        }
    }
    return {};
}

bool resultMatches(const json& want, const std::string& got) {
    if (want.is_array()) {
        for (const auto& w : want) {
            if (w.is_string() && w.get<std::string>() == got) {
                return true;
            }
        }
        return false;
    }
    return want.is_string() && want.get<std::string>() == got;
}

// Where the running approachDummy puts our Link, fixed on its first call.
cXyz sApproachDest = cXyz::Zero;
s16 sApproachYaw = 0;

bool approachDummy(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    fopAc_ac_c* dummy = firstPeerDummy();
    if (link == nullptr || dummy == nullptr) {
        ctx.fail("approachDummy: no player or no dummy for a peer in our layer");
        return false;
    }
    const float dist = ctx.step.value("dist", 110.0f);
    if (!ctx.begun) {
        const s16 yaw = dummy->shape_angle.y;
        sApproachDest.set(dummy->current.pos.x + cM_ssin(yaw) * dist, dummy->current.pos.y,
            dummy->current.pos.z + cM_scos(yaw) * dist);
        sApproachYaw = static_cast<s16>(yaw + 0x8000);
        const cXyz delta = sApproachDest - link->current.pos;
        if (camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0))) {
            camera->mCamera.Reset(camera->mCamera.mCenter + delta, camera->mCamera.mEye + delta);
        }
    }
    // Held while settling: a proc that was turning Link would carry on from the old facing.
    if (ctx.ticks < 10) {
        link->setPlayerPosAndAngle(&sApproachDest, sApproachYaw, TRUE);
        link->speedF = 0.0f;
        link->mNormalSpeed = 0.0f;
        return false;
    }
    const cXyz toDummy = dummy->current.pos - link->current.pos;
    const float d = toDummy.absXZ();
    const int facingError = std::abs(static_cast<s16>(toDummy.atan2sX_Z() - link->shape_angle.y));
    TwiliLog.info("[autotest] approached dummy: {:.0f} units, facing off by 0x{:X} (proc 0x{:X}, "
                  "equip 0x{:X})",
        d, facingError, link->mProcID, static_cast<int>(link->mEquipItem));
    if (d > dist * 2.0f + 30.0f || facingError > 0x2000) {
        ctx.fail(fmt::format("approachDummy: ended {:.0f} units from the dummy, facing off by "
                             "0x{:X}",
            d, facingError));
        return false;
    }
    return true;
}

bool sendPvpHit(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string target = step.value("target", std::string{});
    const Client* c = findPeer(target);
    fopAc_ac_c* dummy =
        c != nullptr ? Session::instance().dummyActorForClient(c->clientId) : nullptr;
    DummyPlayerDebugInfo info;
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (c == nullptr || dummy == nullptr || player == nullptr ||
        !GetDummyPlayerDebugInfo(dummy, info))
    {
        ctx.fail("sendPvpHit: no player, or no dummy of '" + target + "' here");
        return false;
    }
    pvp::HitReport hit;
    if (!pvp::kindFromName(step.value("kind", std::string("sword")), hit.kind)) {
        ctx.fail("sendPvpHit: unknown kind");
        return false;
    }
    hit.damage = static_cast<uint8_t>(step.value("damage", 2));
    if (!pvp::knockbackFromName(step.value("knockback", std::string("light")), hit.knockback)) {
        ctx.fail("sendPvpHit: unknown knockback");
        return false;
    }
    const cXyz toThem = dummy->current.pos - player->current.pos;
    hit.dirY = static_cast<int16_t>(step.value("dirY", static_cast<int>(toThem.atan2sX_Z())));
    hit.blocked = step.value("blocked", false);
    hit.viewSeq = static_cast<uint32_t>(info.shownSeq);
    const uint32_t hitId = pvp::sendDamagePlayerForTest(c->clientId, hit);
    TwiliLog.info("[autotest] sent PvP hit {} to {} (viewSeq {})", hitId, target, hit.viewSeq);
    return true;
}

bool expectPvpResult(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string target = step.value("target", std::string{});
    const Client* c = findPeer(target);
    const pvp::AttackStats* st = c != nullptr ? pvp::attackStats(c->clientId) : nullptr;
    if (st == nullptr || !st->lastAnswered) {
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectPvpResult: no answer to our last hit on '{}'", target));
        }
        return false;
    }
    TwiliLog.info("[autotest] PvP result on {}: {} '{}' damage {}", target, st->lastResult,
        st->lastReason, st->lastDamage);
    std::string why;
    if (step.contains("result") && !resultMatches(step["result"], st->lastResult)) {
        why = "result " + st->lastResult;
    } else if (step.contains("reason") && step.value("reason", std::string{}) != st->lastReason) {
        why = "reason '" + st->lastReason + "'";
    } else if (step.contains("damage") && step.value("damage", 0) != st->lastDamage) {
        why = fmt::format("damage {}", st->lastDamage);
    }
    if (!why.empty()) {
        ctx.fail("expectPvpResult: got " + why);
        return false;
    }
    return true;
}

bool expectPvpStats(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string target = step.value("target", std::string{});
    const Client* c = findPeer(target);
    const pvp::AttackStats* found = c != nullptr ? pvp::attackStats(c->clientId) : nullptr;
    const pvp::AttackStats st = found != nullptr ? *found : pvp::AttackStats{};
    const std::string why = countMismatch(
        step, {{"sent", st.sent}, {"applied", st.applied}, {"blocked", st.blocked},
                  {"dropped", st.dropped}, {"refused", st.refused}, {"damage", st.damage}});
    if (why.empty()) {
        TwiliLog.info("[autotest] PvP stats on {}: sent {} applied {} blocked {} dropped {} "
                      "refused {} damage {}",
            target, st.sent, st.applied, st.blocked, st.dropped, st.refused, st.damage);
        return true;
    }
    if (ctx.seconds >= ctx.timeout(0.0)) {
        ctx.fail(fmt::format("expectPvpStats {}: {} (last result {} '{}')", target, why,
            st.lastResult, st.lastReason));
    }
    return false;
}

bool expectPvpTaken(StepContext& ctx) {
    const json& step = ctx.step;
    const pvp::VictimStats& st = pvp::victimStats();
    std::string why = countMismatch(step, {{"count", st.taken}, {"blocked", st.blocked},
                                              {"dropped", st.dropped}, {"damage", st.damage}});
    if (why.empty() && step.contains("reason") &&
        step.value("reason", std::string{}) != st.lastDropReason)
    {
        why = "last drop reason '" + st.lastDropReason + "'";
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] PvP taken {} blocked {} dropped {} damage {}", st.taken,
            st.blocked, st.dropped, st.damage);
        return true;
    }
    if (ctx.seconds >= ctx.timeout(0.0)) {
        ctx.fail("expectPvpTaken: " + why);
    }
    return false;
}

bool expectReaction(StepContext& ctx) {
    const std::string want = ctx.step.value("knockback", std::string("light"));
    const daAlink_c* link = daAlink_getAlinkActorClass();
    const char* seen = nullptr;
    if (link != nullptr && link->checkCameraLargeDamage()) {
        seen = "knockdown";
    } else if (link != nullptr && (link->mProcID == daAlink_c::PROC_DAMAGE ||
                                      link->mProcID == daAlink_c::PROC_WOLF_DAMAGE))
    {
        seen = "light";
    }
    if (want == "none") {
        if (seen == nullptr && link != nullptr && link->mDamageTimer == 0) {
            return true;
        }
    } else if (seen != nullptr) {
        if (want != seen) {
            ctx.fail(fmt::format("expectReaction: our player plays a {} reaction", seen));
            return false;
        }
        TwiliLog.info("[autotest] reaction {} (proc 0x{:X})", seen, link->mProcID);
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(want == "none" ? fmt::format("expectReaction: still a {} reaction or i-frames",
                                      seen != nullptr ? seen : "no") :
                                  "expectReaction: no " + want + " reaction");
    }
    return false;
}

bool expectDummyHurtbox(StepContext& ctx) {
    const bool want = ctx.step.value("registered", true);
    fopAc_ac_c* dummy = firstPeerDummy();
    DummyPlayerDebugInfo info;
    const bool have = dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info);
    if (have && info.hurtbox == want &&
        (!ctx.step.contains("guard") || ctx.step.value("guard", false) == info.hurtboxGuard))
    {
        return true;
    }
    if (ctx.seconds >= ctx.timeout(0.0)) {
        ctx.fail(have ? fmt::format("expectDummyHurtbox: registered {} guard {}", info.hurtbox,
                            info.hurtboxGuard) :
                        "expectDummyHurtbox: no dummy for a peer in our layer");
    }
    return false;
}

std::optional<bool> pvpSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "setLife") {
        dComIfGp_clearItemLifeCount();
        dComIfGs_setLife(static_cast<u16>(step.value("value", 12)));
        return true;
    }

    if (op == "markLife") {
        sMarkedLife = currentLife();
        TwiliLog.info("[autotest] life marked at {}", sMarkedLife);
        return true;
    }

    if (op == "expectLife") {
        const int life = currentLife();
        const bool ok = (!step.contains("value") || life == step.value("value", 0)) &&
                        (!step.contains("min") || life >= step.value("min", 0)) &&
                        (!step.contains("max") || life <= step.value("max", 0));
        if (ok) {
            TwiliLog.info("[autotest] life is {}", life);
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectLife: life is {} ({})", life, step.dump()));
        }
        return false;
    }

    if (op == "expectLifeDelta") {
        const int want = step.value("delta", 0);
        const int delta = currentLife() - sMarkedLife;
        if (want == 0) {
            if (delta != 0) {
                ctx.fail(fmt::format("expectLifeDelta: life changed by {}", delta));
                return false;
            }
            return ctx.ticks >= step.value("frames", 60);
        }
        if (delta == want) {
            TwiliLog.info("[autotest] life changed by {}", delta);
            return true;
        }
        if ((want < 0 && delta < want) || (want > 0 && delta > want)) {
            ctx.fail(fmt::format("expectLifeDelta: life changed by {}, want {}", delta, want));
            return false;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectLifeDelta: life changed by {}, want {}", delta, want));
        }
        return false;
    }

    if (op == "approachDummy") {
        return approachDummy(ctx);
    }
    if (op == "sendPvpHit") {
        return sendPvpHit(ctx);
    }
    if (op == "expectPvpResult") {
        return expectPvpResult(ctx);
    }
    if (op == "expectPvpStats") {
        return expectPvpStats(ctx);
    }
    if (op == "expectPvpTaken") {
        return expectPvpTaken(ctx);
    }
    if (op == "expectReaction") {
        return expectReaction(ctx);
    }
    if (op == "expectDummyHurtbox") {
        return expectDummyHurtbox(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&pvpSteps);

}  // namespace
}  // namespace twili::autotest
