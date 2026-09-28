// Steps for smooth remote motion and hiding players in cutscenes; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "presence/RemotePose.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_event.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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
            if (fopAc_ac_c* dummy = b.dummyActorForClient(id)) return dummy;
        }
    }
    return nullptr;
}

float distance(const cXyz& a, const cXyz& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Per-call state of the running measureDummyMotion / expectDummyTeleport step.
struct DummyProbe {
    int lastTicks = -1;
    bool havePos = false;
    cXyz lastPos = cXyz::Zero;
    std::vector<float> steps;
    double lastSeq = -1.0;
    int playoutStalls = 0;
    int playoutBursts = 0;
};
DummyProbe sProbe;

// True on the first call after a game tick passed
bool newTick(const StepContext& ctx) {
    if (!ctx.begun) {
        sProbe = DummyProbe{};
    }
    if (ctx.ticks == sProbe.lastTicks) {
        return false;
    }
    sProbe.lastTicks = ctx.ticks;
    return true;
}

bool poseSelfTest(StepContext& ctx) {
    std::string why;
    if (!runRemotePoseSelfTest(why)) {
        ctx.fail("poseSelfTest: " + why);
        return false;
    }
    struct Row {
        bool running;
        uint8_t mode;
        uint16_t flags;
        bool cutscene;
        const char* what;
    };
    static constexpr Row kRows[] = {
        {false, dEvt_mode_DEMO_e, 0, false, "no event"},
        {true, dEvt_mode_TALK_e, 0, false, "talk"},
        {true, dEvt_mode_DEMO_e, 0, true, "demo"},
        {true, dEvt_mode_DEMO_e, 0x40, false, "door"},
        {true, dEvt_mode_DEMO_e, 4, true, "chest"},
        {true, dEvt_mode_COMPULSORY_e, 0, false, "compulsory"},
        {true, dEvt_mode_WAIT_e, 0, false, "ending"},
    };
    for (const Row& row : kRows) {
        if (isCutsceneEvent(row.running, row.mode, row.flags) != row.cutscene) {
            ctx.fail(fmt::format("poseSelfTest: a {} event {} as a cutscene", row.what,
                                 row.cutscene ? "does not count" : "counts"));
            return false;
        }
    }
    TwiliLog.info("[autotest] pose self-test passed");
    return true;
}

bool measureDummyMotion(StepContext& ctx) {
    const int frames = ctx.step.value("frames", 240);
    const int settle = ctx.step.value("settleFrames", 20);
    if (!newTick(ctx) || ctx.ticks <= settle) {
        return false;
    }
    const bool self = ctx.step.value("self", false);
    fopAc_ac_c* dummy = self ? dComIfGp_getPlayer(0) : firstPeerDummy();
    if (dummy == nullptr) {
        ctx.fail(self ? "measureDummyMotion: no local player"
                      : "measureDummyMotion: no dummy for a peer in our layer");
        return false;
    }
    // fopAc_Execute and applyRemoteState set old.pos to the position before this tick.
    sProbe.steps.push_back(distance(dummy->current.pos, dummy->old.pos));
    DummyPlayerDebugInfo info;
    if (!self && GetDummyPlayerDebugInfo(dummy, info)) {
        if (sProbe.lastSeq >= 0.0) {
            const double advance = info.shownSeq - sProbe.lastSeq;
            if (advance < 0.5) {
                sProbe.playoutStalls++;
            } else if (advance > 1.5) {
                sProbe.playoutBursts++;
            }
        }
        sProbe.lastSeq = info.shownSeq;
    }
    if (ctx.ticks < settle + frames) {
        return false;
    }

    std::vector<float> sorted = sProbe.steps;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2];
    int stalls = 0;
    int bursts = 0;
    for (float step : sProbe.steps) {
        if (step < 0.25f * median) {
            stalls++;
        } else if (step > 1.75f * median) {
            bursts++;
        }
    }
    const float badRatio = static_cast<float>(stalls + bursts) / sProbe.steps.size();
    const float playoutBadRatio =
        static_cast<float>(sProbe.playoutStalls + sProbe.playoutBursts) / sProbe.steps.size();
    TwiliLog.info("[autotest] {} motion samples={} median={:.2f} min={:.2f} max={:.2f} "
                 "stalls={} bursts={} badRatio={:.3f} playoutStalls={} playoutBursts={} "
                 "playoutBadRatio={:.3f}",
                 self ? "self" : "dummy", sProbe.steps.size(), median, sorted.front(),
                 sorted.back(), stalls, bursts, badRatio, sProbe.playoutStalls,
                 sProbe.playoutBursts, playoutBadRatio);
    const float minSpeed = ctx.step.value("minSpeed", 3.0f);
    const float maxBadRatio = ctx.step.value("maxBadRatio", 0.03f);
    if (median < minSpeed) {
        ctx.fail(fmt::format("measureDummyMotion: median step {:.2f} under {:.2f}: the peer "
                             "was not moving", median, minSpeed));
        return false;
    }
    if (!self && playoutBadRatio > maxBadRatio) {
        ctx.fail(fmt::format("measureDummyMotion: playout stalled {} and jumped {} times in {} "
                             "ticks ({:.3f} > {:.3f})", sProbe.playoutStalls,
                             sProbe.playoutBursts, sProbe.steps.size(), playoutBadRatio,
                             maxBadRatio));
        return false;
    }
    return true;
}

bool expectDummyTeleport(StepContext& ctx) {
    const float minDist = ctx.step.value("minDist", 1500.0f);
    const float maxPreStep = ctx.step.value("maxPreStep", 50.0f);
    if (newTick(ctx)) {
        if (fopAc_ac_c* dummy = firstPeerDummy()) {
            const float moved =
                sProbe.havePos ? distance(dummy->current.pos, sProbe.lastPos) : 0.0f;
            sProbe.lastPos = dummy->current.pos;
            sProbe.havePos = true;
            if (moved >= minDist) {
                TwiliLog.info("[autotest] dummy jumped {:.0f} units in one frame", moved);
                return true;
            }
            if (moved > maxPreStep) {
                ctx.fail(fmt::format("expectDummyTeleport: the dummy moved {:.0f} units in one "
                                     "frame before the jump: a partial slide, or the peer was "
                                     "not standing still", moved));
                return false;
            }
        }
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("expectDummyTeleport: no jump of {:.0f} units", minDist));
    }
    return false;
}

std::optional<bool> motionSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;
    auto& b = Session::instance();

    if (op == "poseSelfTest") {
        return poseSelfTest(ctx);
    }

    if (op == "waitRoomState") {
        const std::string key = step.value("key", std::string{});
        const json room = b.roomState().toJson();
        const auto it = room.find(key);
        if (it != room.end() && *it == step.value("value", json())) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("waitRoomState: {} is {}, want {}", key,
                                 it != room.end() ? it->dump() : "missing",
                                 step.value("value", json()).dump()));
        }
        return false;
    }

    if (op == "forceCutscene") {
        const json on = step.value("on", json());
        setCutsceneOverrideForTest(on.is_boolean() ? (on.get<bool>() ? 1 : 0) : -1);
        return true;
    }

    if (op == "beginEvent" || op == "endEvent") {
        const bool begin = op == "beginEvent";
        if (!ctx.begun) {
            if (begin) {
                const std::string kind = step.value("kind", std::string("compulsory"));
                fopAc_ac_c* player = dComIfGp_getPlayer(0);
                if (kind != "compulsory" || player == nullptr) {
                    ctx.fail(player == nullptr ? "beginEvent: no player"
                                               : "beginEvent: unknown kind '" + kind + "'");
                    return false;
                }
                dComIfGp_event_compulsory(player, nullptr, 0xFFFF);
            } else {
                dComIfGp_event_reset();
            }
        }
        const bool running = dComIfGp_event_runCheck() != FALSE;
        if (begin ? running && dComIfGp_event_getMode() == dEvt_mode_COMPULSORY_e : !running) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("{}: event running={} mode={}", op, running,
                                 dComIfGp_event_getMode()));
        }
        return false;
    }

    if (op == "expectDummyHidden") {
        const bool want = step.value("hidden", true);
        int peers = 0;
        std::string why;
        for (const auto& [id, c] : b.clients()) {
            if (!peerInMyLayer(c)) continue;
            peers++;
            fopAc_ac_c* dummy = b.dummyActorForClient(id);
            if (dummy == nullptr) {
                why = fmt::format("client {} ({}) has no dummy", id, c.name);
            } else if (IsDummyPlayerHidden(dummy) != want) {
                why = fmt::format("the dummy of client {} ({}) is {}", id, c.name,
                                  want ? "shown" : "hidden");
            }
        }
        if (peers > 0 && why.empty()) {
            return true;
        }
        if (ctx.seconds >= ctx.timeout(0.0)) {
            ctx.fail("expectDummyHidden: " + (peers == 0 ? "no peer in our layer" : why));
        }
        return false;
    }

    if (op == "expectPeerFlag") {
        const std::string flag = step.value("flag", std::string("inCutscene"));
        const uint8_t bit = flag == "inCutscene" ? kPresenceInCutscene :
                            flag == "wolf"       ? kPresenceWolf :
                                                   0;
        if (bit == 0) {
            ctx.fail("expectPeerFlag: unknown flag '" + flag + "'");
            return false;
        }
        const bool want = step.value("set", true);
        int peers = 0;
        std::string why;
        for (const auto& [id, c] : b.clients()) {
            if (!peerInMyLayer(c)) continue;
            peers++;
            if (((c.presenceFlags & bit) != 0) != want) {
                why = fmt::format("client {} ({}) has {} {}", id, c.name, flag,
                                  want ? "clear" : "set");
            }
        }
        if (peers > 0 && why.empty()) {
            return true;
        }
        if (ctx.seconds >= ctx.timeout(0.0)) {
            ctx.fail("expectPeerFlag: " + (peers == 0 ? "no peer in our layer" : why));
        }
        return false;
    }

    if (op == "measureDummyMotion") {
        return measureDummyMotion(ctx);
    }

    if (op == "dumpDummies") {
        auto& b = Session::instance();
        const char* stage = dComIfGp_getStartStageName();
        for (const auto& [id, c] : b.clients()) {
            if (c.self) continue;
            fopAc_ac_c* dummy = b.dummyActorForClient(id);
            DummyPlayerDebugInfo info;
            const bool haveInfo = dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info);
            TwiliLog.info(
                "[autotest] dummy client {} ({}): stage {} layer {} (ours {} {}) update {} poses {} "
                "reported ({:.0f} {:.0f} {:.0f}) dummy {} ({:.0f} {:.0f} {:.0f}) shownSeq {:.1f} "
                "ready {} hidden {} wolf {}/{}",
                id, c.name, c.stageName, c.layerNo, stage != nullptr ? stage : "-",
                dComIfG_play_c::getLayerNo(0), c.hasPlayerUpdate, c.pose.size(), c.posX, c.posY,
                c.posZ, dummy != nullptr, dummy != nullptr ? dummy->current.pos.x : 0.0f,
                dummy != nullptr ? dummy->current.pos.y : 0.0f,
                dummy != nullptr ? dummy->current.pos.z : 0.0f, haveInfo ? info.shownSeq : -1.0,
                haveInfo && info.shellReady, haveInfo && info.hidden, haveInfo && info.remoteWolf,
                haveInfo && info.wolfBody);
        }
        return true;
    }

    if (op == "teleportSelf") {
        daPy_py_c* player = dComIfGp_getLinkPlayer();
        if (player == nullptr) {
            ctx.fail("teleportSelf: no player");
            return false;
        }
        cXyz pos = player->current.pos;
        pos.x += step.value("dx", 0.0f);
        pos.y += step.value("dy", 0.0f);
        pos.z += step.value("dz", 0.0f);
        player->setPlayerPosAndAngle(&pos, player->shape_angle.y, TRUE);
        interp::requestPresentationSync();
        TwiliLog.info("[autotest] moved to {:.0f} {:.0f} {:.0f}", pos.x, pos.y, pos.z);
        return true;
    }

    if (op == "expectDummyTeleport") {
        return expectDummyTeleport(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&motionSteps);

}  // namespace
}  // namespace twili::autotest

