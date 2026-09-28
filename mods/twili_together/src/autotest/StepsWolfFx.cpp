// Steps for a remote wolf's attack effects (fx/WolfFx.cpp, the "wx" group of PLAYER_UPDATE,
// actors/DummyWolfFx.cpp). spin fields take none | left | right (| any for the local capture).
//
// wolfSpin  dir (right | left), trail (true), timeoutSec (10)
//     procWolfRollAttackInit on the local wolf, again on every call until the proc takes.
// wolfDome  force (true), radius (550), minLocks (0), timeoutSec (20)
//     Needs B held by an async walk before it. With force, opens the dome once the charge ran
//     out in PROC_WOLF_ROLL_ATTACK_MOVE (a light-world Midna is tired and never opens it).
// expectLocalWolfFx  spin, charge, dome, minRadius, lockBlur, timeoutSec (1)
// expectPeerWolfFx  spin, dome, minRadius, lockBlur, lockDashSeq, hairAim, timeoutSec (10)
//     What the first peer in our layer last sent. hairAim: false, or an angle (within 0x100).
// expectRemoteWolfFx  spin, lastSpin, minSpinTicks, maxSpinTicks, spinEmitters, dome, domeShown,
//                     minRadius, maxRadius, lockBlur, minLockDashes, maxLockDashes, hairAim,
//                     count (1), frames (0), timeoutSec (10)
//     What the peers' dummies show; spin ticks and lock dashes count from the step's first call.
// viewDummy  back (250), side (0), up (250)
//     Our camera behind our Link, looking between him and the first peer's dummy.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "fx/WolfFx.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cstdlib>
#include <cstring>
#include <map>

namespace twili::autotest {
namespace {

using nlohmann::json;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

// What PLAYER_UPDATE would send of the local wolf this tick.
RemoteWolfFx captureNow() {
    RemoteMidnaPose midna;
    Session::captureLocalMidna(localCutsceneRunning(), midna);
    RemoteWolfFx fx;
    wolffx::captureLocal(midna, fx);
    return fx;
}

// 0 none, 1 left, 2 right.
uint8_t spinOf(const RemoteWolfFx& fx) {
    if (!(fx.flags & kWolfFxSpin)) {
        return 0;
    }
    return (fx.flags & kWolfFxSpinRight) ? 2 : 1;
}

const char* spinName(uint8_t spin) {
    return spin == 2 ? "right" : spin == 1 ? "left" : "none";
}

// A missing key matches; an unknown name never does.
bool spinMatches(const json& step, const char* key, uint8_t spin) {
    if (!step.contains(key)) {
        return true;
    }
    const std::string want = step[key].get<std::string>();
    return want == "any" ? spin != 0 : want == spinName(spin);
}

bool hairAimMatches(const json& step, bool valid, int16_t angle) {
    if (!step.contains("hairAim")) {
        return true;
    }
    const json& want = step["hairAim"];
    if (want.is_boolean()) {
        return want.get<bool>() == valid;
    }
    const int16_t d = static_cast<int16_t>(angle - static_cast<int16_t>(want.get<int>()));
    return valid && std::abs(d) <= 0x100;
}

bool attackProc(int proc) {
    return proc == daAlink_c::PROC_WOLF_ROLL_ATTACK ||
           proc == daAlink_c::PROC_WOLF_ROLL_ATTACK_CHARGE ||
           proc == daAlink_c::PROC_WOLF_ROLL_ATTACK_MOVE ||
           proc == daAlink_c::PROC_WOLF_LOCK_ATTACK ||
           proc == daAlink_c::PROC_WOLF_LOCK_ATTACK_TURN;
}

bool sSpinStarted = false;

bool wolfSpin(StepContext& ctx) {
    const std::string dirName = ctx.step.value("dir", std::string("right"));
    if (dirName != "right" && dirName != "left") {
        ctx.fail("wolfSpin: unknown dir '" + dirName + "'");
        return false;
    }
    const bool right = dirName == "right";
    const bool trail = ctx.step.value("trail", true);
    if (!ctx.begun) {
        sSpinStarted = false;
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || !link->checkWolf()) {
        ctx.fail("wolfSpin: the local player is no wolf");
        return false;
    }
    if (!sSpinStarted) {
        if (link->mLinkAcch.ChkGroundHit() && !attackProc(link->mProcID)) {
            link->procWolfRollAttackInit(right ? 1 : 0, trail ? 0 : 2);
        }
        sSpinStarted = link->mProcID == daAlink_c::PROC_WOLF_ROLL_ATTACK;
    }
    const RemoteWolfFx fx = captureNow();
    const bool noTrail = (fx.flags & kWolfFxSpinNoTrail) != 0;
    if (sSpinStarted && spinOf(fx) == (right ? 2 : 1) && noTrail == !trail) {
        TwiliLog.info("[autotest] local wolf spin {} trail {} (seq {})", dirName, trail,
            Session::localPoseSeq());
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format("wolfSpin: proc {} ground {} capture flags 0x{:X}",
            static_cast<int>(link->mProcID), static_cast<bool>(link->mLinkAcch.ChkGroundHit()),
            fx.flags));
    }
    return false;
}

bool sDomeForced = false;

bool wolfDome(StepContext& ctx) {
    if (!ctx.begun) {
        sDomeForced = false;
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || !link->checkWolf()) {
        ctx.fail("wolfDome: the local player is no wolf");
        return false;
    }
    const bool force = ctx.step.value("force", true);
    if (force && !sDomeForced && link->mProcID == daAlink_c::PROC_WOLF_ROLL_ATTACK_MOVE &&
        link->mProcVar0.field_0x3008 == 0 && link->mEquipItem != wolffx::kLockDomeItem &&
        !link->checkWolfLockAttackChargeState())
    {
        link->setWolfLockDomeModel();
        sDomeForced = true;
        TwiliLog.info("[autotest] local wolf dome forced open");
    }
    const RemoteWolfFx fx = captureNow();
    const bool dome = (fx.flags & kWolfFxDome) != 0;
    if (dome && fx.domeRadius >= ctx.step.value("radius", 550.0f) &&
        fx.lockCount >= ctx.step.value("minLocks", 0))
    {
        TwiliLog.info("[autotest] local wolf dome radius {:.0f}, {} locks, forced {} (seq {})",
            fx.domeRadius, fx.lockCount, sDomeForced, Session::localPoseSeq());
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("wolfDome: proc {} charge {} dome {} radius {:.0f} locks {}",
            static_cast<int>(link->mProcID), link->mProcVar0.field_0x3008, dome, fx.domeRadius,
            fx.lockCount));
    }
    return false;
}

bool expectLocalWolfFx(StepContext& ctx) {
    const json& step = ctx.step;
    const RemoteWolfFx fx = captureNow();
    std::string why;
    if (!spinMatches(step, "spin", spinOf(fx))) {
        why = fmt::format("spin is {}", spinName(spinOf(fx)));
    }
    auto want = [&](const char* key, bool have) {
        if (why.empty() && step.contains(key) && step[key].get<bool>() != have) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    want("charge", (fx.flags & kWolfFxCharge) != 0);
    want("dome", (fx.flags & kWolfFxDome) != 0);
    want("lockBlur", (fx.flags & kWolfFxLockBlur) != 0);
    if (why.empty() && step.contains("minRadius") && fx.domeRadius < step.value("minRadius", 0.0f))
    {
        why = fmt::format("dome radius is {:.0f}", fx.domeRadius);
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] local wolf fx: flags 0x{:X}, radius {:.0f}, dash seq {}, {} "
                      "locks",
            fx.flags, fx.domeRadius, fx.lockDashSeq, fx.lockCount);
        return true;
    }
    if (ctx.seconds >= ctx.timeout(1.0)) {
        ctx.fail("expectLocalWolfFx: " + why);
    }
    return false;
}

bool expectPeerWolfFx(StepContext& ctx) {
    const json& step = ctx.step;
    std::string why = "no peer in our layer";
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!peerInMyLayer(c)) {
            continue;
        }
        const RemoteWolfFx& fx = c.wolfFx;
        why.clear();
        if (!spinMatches(step, "spin", spinOf(fx))) {
            why = fmt::format("spin is {}", spinName(spinOf(fx)));
        }
        auto want = [&](const char* key, bool have) {
            if (why.empty() && step.contains(key) && step[key].get<bool>() != have) {
                why = fmt::format("{} is {}", key, have);
            }
        };
        want("dome", (fx.flags & kWolfFxDome) != 0);
        want("lockBlur", (fx.flags & kWolfFxLockBlur) != 0);
        if (why.empty() && step.contains("minRadius") &&
            fx.domeRadius < step.value("minRadius", 0.0f))
        {
            why = fmt::format("dome radius is {:.0f}", fx.domeRadius);
        }
        if (why.empty() && step.contains("lockDashSeq") &&
            fx.lockDashSeq != step.value("lockDashSeq", 0))
        {
            why = fmt::format("dash seq is {}", fx.lockDashSeq);
        }
        if (why.empty() && !hairAimMatches(step, c.midna.hairAimValid, c.midna.hairAim)) {
            why = fmt::format("hair aim is {} 0x{:X}", c.midna.hairAimValid,
                static_cast<uint16_t>(c.midna.hairAim));
        }
        if (why.empty()) {
            TwiliLog.info("[autotest] peer wolf fx: client {} flags 0x{:X}, radius {:.0f}, dash "
                          "seq {}, {} locks",
                id, fx.flags, fx.domeRadius, fx.lockDashSeq, fx.lockCount);
            return true;
        }
        why = fmt::format("client {} ({}): {}", id, c.name, why);
        break;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectPeerWolfFx: " + why);
    }
    return false;
}

// Where the running expectRemoteWolfFx counts from, per client.
struct WolfFxBase {
    uint32_t spinTicks = 0;
    uint32_t lockDashes = 0;
};
std::map<uint32_t, WolfFxBase> sWolfFxBase;
// Tick at which it first matched, -1 before.
int sWolfFxMatchedAt = -1;

std::string wolfFxMismatch(const json& step, const DummyPlayerDebugInfo& info,
    const WolfFxBase& base) {
    std::string why;
    if (!spinMatches(step, "spin", info.wolfSpin)) {
        return fmt::format("spin is {}", spinName(info.wolfSpin));
    }
    if (!spinMatches(step, "lastSpin", info.wolfLastSpin)) {
        return fmt::format("last spin is {}", spinName(info.wolfLastSpin));
    }
    const double spinTicks = info.wolfSpinTicks - base.spinTicks;
    const double dashes = info.wolfLockDashes - base.lockDashes;
    auto want = [&](const char* key, auto have) {
        if (why.empty() && step.contains(key) && step[key].get<decltype(have)>() != have) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    auto atLeast = [&](const char* key, double have) {
        if (why.empty() && step.contains(key) && have < step[key].get<double>()) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    auto atMost = [&](const char* key, double have) {
        if (why.empty() && step.contains(key) && have > step[key].get<double>()) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    atLeast("minSpinTicks", spinTicks);
    atMost("maxSpinTicks", spinTicks);
    want("spinEmitters", static_cast<int>(info.wolfSpinEmitters));
    want("dome", info.wolfDome);
    want("domeShown", info.wolfDomeShown);
    atLeast("minRadius", info.wolfDomeRadius);
    atMost("maxRadius", info.wolfDomeRadius);
    want("lockBlur", info.wolfLockBlurAlpha != 0);
    atLeast("minLockDashes", dashes);
    atMost("maxLockDashes", dashes);
    if (why.empty() && !hairAimMatches(step, info.midnaHairAim, info.midnaHairAimAngle)) {
        why = fmt::format("hair aim is {} 0x{:X}", info.midnaHairAim,
            static_cast<uint16_t>(info.midnaHairAimAngle));
    }
    return why;
}

bool expectRemoteWolfFx(StepContext& ctx) {
    auto& b = Session::instance();
    if (!ctx.begun) {
        sWolfFxMatchedAt = -1;
        sWolfFxBase.clear();
    }
    int matching = 0;
    std::string why = "no peer in our layer";
    std::string seen;
    for (const auto& [id, c] : b.clients()) {
        if (!peerInMyLayer(c)) {
            continue;
        }
        DummyPlayerDebugInfo info;
        fopAc_ac_c* dummy = b.dummyActorForClient(id);
        if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info) || !info.shellReady) {
            why = fmt::format("client {} ({}) has no dummy", id, c.name);
            continue;
        }
        // Counters start at the first sight of each dummy; a recreated one counts from 0.
        auto it = sWolfFxBase.find(id);
        if (it == sWolfFxBase.end()) {
            it = sWolfFxBase.emplace(id, WolfFxBase{info.wolfSpinTicks, info.wolfLockDashes}).first;
        }
        WolfFxBase& base = it->second;
        if (info.wolfSpinTicks < base.spinTicks || info.wolfLockDashes < base.lockDashes) {
            base = WolfFxBase{};
        }
        const std::string miss = wolfFxMismatch(ctx.step, info, base);
        seen = fmt::format("spin {} (last {}, {} ticks, {} emitters), dome {} shown {} radius "
                           "{:.0f}, blur {}, {} dashes, hair aim {} 0x{:X}",
            spinName(info.wolfSpin), spinName(info.wolfLastSpin),
            info.wolfSpinTicks - base.spinTicks, info.wolfSpinEmitters, info.wolfDome,
            info.wolfDomeShown, info.wolfDomeRadius, info.wolfLockBlurAlpha,
            info.wolfLockDashes - base.lockDashes, info.midnaHairAim,
            static_cast<uint16_t>(info.midnaHairAimAngle));
        if (miss.empty()) {
            matching++;
            continue;
        }
        why = fmt::format("client {} ({}): {} ({})", id, c.name, miss, seen);
    }
    if (matching >= ctx.step.value("count", 1)) {
        if (sWolfFxMatchedAt < 0) {
            sWolfFxMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sWolfFxMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] remote wolf fx on {} peer(s): {}", matching, seen);
        return true;
    }
    if (sWolfFxMatchedAt >= 0) {
        ctx.fail(fmt::format("expectRemoteWolfFx: {} ticks after it matched, {}",
            ctx.ticks - sWolfFxMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectRemoteWolfFx: " + why);
    }
    return false;
}

bool viewDummy(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    fopAc_ac_c* dummy = nullptr;
    auto& b = Session::instance();
    for (const auto& [id, c] : b.clients()) {
        if (peerInMyLayer(c) && (dummy = b.dummyActorForClient(id)) != nullptr) {
            break;
        }
    }
    camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
    if (link == nullptr || dummy == nullptr || camera == nullptr) {
        ctx.fail("viewDummy: no player, camera or dummy for a peer in our layer");
        return false;
    }
    cXyz dir = dummy->current.pos - link->current.pos;
    dir.y = 0.0f;
    if (dir.absXZ() < 1.0f) {
        return true;
    }
    dir.normalize();
    const cXyz center = (link->current.pos + dummy->current.pos) * 0.5f + cXyz(0.0f, 50.0f, 0.0f);
    const cXyz right(dir.z, 0.0f, -dir.x);
    const cXyz eye = link->current.pos - dir * ctx.step.value("back", 250.0f) +
                     right * ctx.step.value("side", 0.0f) +
                     cXyz(0.0f, ctx.step.value("up", 250.0f), 0.0f);
    camera->mCamera.Reset(center, eye);
    return true;
}

std::optional<bool> wolfFxSteps(const std::string& op, StepContext& ctx) {
    if (op == "wolfSpin") {
        return wolfSpin(ctx);
    }
    if (op == "wolfDome") {
        return wolfDome(ctx);
    }
    if (op == "expectLocalWolfFx") {
        return expectLocalWolfFx(ctx);
    }
    if (op == "expectPeerWolfFx") {
        return expectPeerWolfFx(ctx);
    }
    if (op == "expectRemoteWolfFx") {
        return expectRemoteWolfFx(ctx);
    }
    if (op == "viewDummy") {
        return viewDummy(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&wolfFxSteps);

}  // namespace
}  // namespace twili::autotest
