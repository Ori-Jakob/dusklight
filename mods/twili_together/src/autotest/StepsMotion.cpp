// Steps for smooth remote motion and hiding players in cutscenes; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "presence/RemotePose.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "SSystem/SComponent/c_math.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
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
        {true, dEvt_mode_DEMO_e, 4, false, "chest"},
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

// The Link the body steps look at: the first peer's dummy, or ours with `self`
daAlink_c* bodyTarget(const StepContext& ctx) {
    if (ctx.step.value("self", false)) {
        return daAlink_getAlinkActorClass();
    }
    return static_cast<daAlink_c*>(firstPeerDummy());
}

struct BodyProbe {
    int lastTicks = -1;
    bool have = false;
    float lastY = 0.0f;
    float lastDy = 0.0f;
    bool haveDy = false;
    cXyz lastPos = cXyz::Zero;
    float travel = 0.0f;
    float maxJerk = 0.0f;
    int samples = 0;
    int jerks = 0;
};
BodyProbe sBody;

bool measureBodyHeight(StepContext& ctx) {
    if (!ctx.begun) {
        sBody = BodyProbe{};
    }
    const int frames = ctx.step.value("frames", 150);
    const int settle = ctx.step.value("settleFrames", 20);
    if (ctx.ticks == sBody.lastTicks || ctx.ticks <= settle) {
        return false;
    }
    sBody.lastTicks = ctx.ticks;
    daAlink_c* link = bodyTarget(ctx);
    if (link == nullptr || link->mpLinkModel == nullptr) {
        ctx.fail("measureBodyHeight: no player or no dummy for a peer in our layer");
        return false;
    }
    // The body matrix: position plus setMatrix's and footBgCheck's offsets
    const float y = link->mpLinkModel->getBaseTRMtx()[1][3];
    const float jerk = ctx.step.value("jerk", 5.0f);
    if (sBody.have) {
        const float dy = y - sBody.lastY;
        if (sBody.haveDy) {
            const float ddy = std::fabs(dy - sBody.lastDy);
            sBody.maxJerk = std::max(sBody.maxJerk, ddy);
            if (ddy > jerk) {
                sBody.jerks++;
            }
        }
        sBody.lastDy = dy;
        sBody.haveDy = true;
        sBody.travel += std::hypot(link->current.pos.x - sBody.lastPos.x,
                                   link->current.pos.z - sBody.lastPos.z);
        sBody.samples++;
    }
    sBody.have = true;
    sBody.lastY = y;
    sBody.lastPos = link->current.pos;
    if (ctx.ticks < settle + frames) {
        return false;
    }
    const float ratio = sBody.samples > 0 ? static_cast<float>(sBody.jerks) / sBody.samples : 0.0f;
    TwiliLog.info("[autotest] {} body height samples={} travel={:.0f} jerks={} (>{:.1f}) "
                  "ratio={:.3f} maxJerk={:.2f}",
                  ctx.step.value("self", false) ? "self" : "dummy", sBody.samples, sBody.travel,
                  sBody.jerks, jerk, ratio, sBody.maxJerk);
    if (sBody.travel < ctx.step.value("minTravel", 60.0f)) {
        ctx.fail(fmt::format("measureBodyHeight: the body moved {:.0f}: nobody walked",
                             sBody.travel));
        return false;
    }
    if (ratio > ctx.step.value("maxJerkRatio", 0.05f)) {
        ctx.fail(fmt::format("measureBodyHeight: the body's climb rate jumped by more than {:.1f} "
                             "on {} of {} ticks (worst {:.1f})", jerk, sBody.jerks, sBody.samples,
                             sBody.maxJerk));
        return false;
    }
    return true;
}

// A spot on a steady slope near our Link, where it then stands across the slope.
bool findSlope(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("findSlope: no player");
        return false;
    }
    const float minNy = std::cos(ctx.step.value("maxDeg", 30.0f) * 3.14159265f / 180.0f);
    const float maxNy = std::cos(ctx.step.value("minDeg", 12.0f) * 3.14159265f / 180.0f);
    const float radius = ctx.step.value("radius", 1200.0f);
    const float step = ctx.step.value("step", 60.0f);
    const cXyz home = link->current.pos;
    dBgS_LinkGndChk chk;
    auto groundAt = [&](float x, float z, cM3dGPla* plane) {
        cXyz start(x, home.y + 300.0f, z);
        chk.SetPos(&start);
        const float y = dComIfG_Bgsp().GroundCross(&chk);
        if (y == -G_CM3D_F_INF || (plane != nullptr && !dComIfG_Bgsp().GetTriPla(chk, plane))) {
            return -G_CM3D_F_INF;
        }
        return y;
    };
    float bestDist = FLT_MAX;
    cXyz best;
    cXyz bestNormal;
    int hits = 0;
    int level = 0;
    int sloped = 0;
    float steepest = 0.0f;
    for (float dx = -radius; dx <= radius; dx += step) {
        for (float dz = -radius; dz <= radius; dz += step) {
            const float x = home.x + dx;
            const float z = home.z + dz;
            cM3dGPla plane;
            const float y = groundAt(x, z, &plane);
            if (y == -G_CM3D_F_INF) {
                continue;
            }
            hits++;
            const float ny = plane.mNormal.y;
            if (std::fabs(y - home.y) > 150.0f) {
                continue;
            }
            level++;
            steepest = std::max(steepest, std::acos(std::min(ny, 1.0f)) * 180.0f / 3.14159265f);
            if (ny < minNy || ny > maxNy) {
                continue;
            }
            sloped++;
            // The same plane 40 units around, not an edge or a step
            bool steady = true;
            for (int k = 0; k < 4 && steady; k++) {
                const float ox = k == 0 ? 40.0f : k == 1 ? -40.0f : 0.0f;
                const float oz = k == 2 ? 40.0f : k == 3 ? -40.0f : 0.0f;
                const float want = y - (plane.mNormal.x * ox + plane.mNormal.z * oz) / ny;
                steady = std::fabs(groundAt(x + ox, z + oz, nullptr) - want) < 3.0f;
            }
            const float dist = std::hypot(dx, dz);
            if (steady && dist < bestDist) {
                bestDist = dist;
                best.set(x, y, z);
                bestNormal = plane.mNormal;
            }
        }
    }
    TwiliLog.info("[autotest] findSlope from ({:.0f} {:.0f} {:.0f}): {} ground hits, {} near our "
                  "height (steepest {:.1f} degrees), {} in the range",
                  home.x, home.y, home.z, hits, level, steepest, sloped);
    if (bestDist == FLT_MAX) {
        ctx.fail("findSlope: no steady slope within reach");
        return false;
    }
    // Facing across it, the feet stand at different heights
    const s16 downhill = cM_atan2s(bestNormal.x, bestNormal.z);
    link->setPlayerPosAndAngle(&best, static_cast<s16>(downhill + 0x4000), TRUE);
    TwiliLog.info("[autotest] findSlope: ({:.0f} {:.0f} {:.0f}) {:.1f} degrees, {:.0f} away",
                  best.x, best.y, best.z, std::acos(bestNormal.y) * 180.0f / 3.14159265f,
                  bestDist);
    return true;
}

// footBgCheck's foot points: the human's heel-toe midpoints, the wolf's paws
int bodyFeet(daAlink_c* link, cXyz out[4]) {
    J3DModel* model = link->mpLinkModel;
    if (link->checkWolf()) {
        static const Vec kPaw = {6.0f, -5.0f, 0.0f};
        static const u16 kPawJoint[4] = {0x13, 0x18, 0x1F, 0x24};
        for (int i = 0; i < 4; i++) {
            mDoMtx_multVec(model->getAnmMtx(kPawJoint[i]), &kPaw, &out[i]);
        }
        return 4;
    }
    static const Vec kHeel[2] = {{-3.0f, 13.0f, 0.0f}, {-3.0f, -13.0f, 0.0f}};
    static const Vec kToe[2] = {{10.0f, 5.0f, 0.0f}, {10.0f, -5.0f, 0.0f}};
    static const u16 kHeelJoint[2] = {0x14, 0x19};
    for (int i = 0; i < 2; i++) {
        cXyz heel;
        cXyz toe;
        mDoMtx_multVec(model->getAnmMtx(kHeelJoint[i]), &kHeel[i], &heel);
        mDoMtx_multVec(model->getAnmMtx(kHeelJoint[i] + 1), &kToe[i], &toe);
        out[i] = (heel + toe) * 0.5f;
    }
    return 2;
}

float legBendDeg(daAlink_c* link) {
    int bend = 0;
    for (int i = 0; i < 2; i++) {
        bend = std::max({bend, std::abs(link->mFootData1[i].field_0x6),
                         std::abs(link->mFootData1[i].field_0x4)});
        if (link->checkWolf()) {
            bend = std::max({bend, std::abs(link->mFootData2[i].field_0x6),
                             std::abs(link->mFootData2[i].field_0x4)});
        }
    }
    return bend * (360.0f / 65536.0f);
}

int sFeetHeldAt = -1;
float sFeetWorstSpread = 0.0f;

bool expectFeetOnGround(StepContext& ctx) {
    if (!ctx.begun) {
        sFeetHeldAt = -1;
        sFeetWorstSpread = 0.0f;
    }
    const bool self = ctx.step.value("self", false);
    daAlink_c* link = bodyTarget(ctx);
    std::string why;
    float spread = 0.0f;
    float bend = 0.0f;
    if (link == nullptr || link->mpLinkModel == nullptr) {
        why = "no player or no dummy for a peer in our layer";
    } else {
        cXyz feet[4];
        const int n = bodyFeet(link, feet);
        float lo = FLT_MAX;
        float hi = -FLT_MAX;
        std::string heights;
        dBgS_LinkGndChk chk;
        for (int i = 0; i < n; i++) {
            cXyz start(feet[i].x, feet[i].y + 50.0f, feet[i].z);
            chk.SetPos(&start);
            const float ground = dComIfG_Bgsp().GroundCross(&chk);
            if (ground == -G_CM3D_F_INF) {
                why = fmt::format("no ground under foot {}", i);
                break;
            }
            lo = std::min(lo, feet[i].y - ground);
            hi = std::max(hi, feet[i].y - ground);
            heights += fmt::format(" {:.1f}", feet[i].y - ground);
        }
        spread = hi - lo;
        bend = legBendDeg(link);
        if (ctx.step.value("trace", false)) {
            TwiliLog.info("[autotest] feet t{} spread {:.1f} bend {:.1f} heights{}", ctx.ticks,
                          spread, bend, heights);
        }
        DummyPlayerDebugInfo info;
        if (!why.empty()) {
            // no ground under a foot
        } else if (spread > ctx.step.value("maxSpread", 4.0f)) {
            why = fmt::format("the feet stand {:.1f} apart over their ground ({:.1f} to {:.1f})",
                              spread, lo, hi);
        } else if (bend < ctx.step.value("minBend", 0.0f)) {
            why = fmt::format("the legs turn {:.1f} degrees at most", bend);
        } else if (!self && GetDummyPlayerDebugInfo(link, info) &&
                   info.feetContact != ctx.step.value("contact", true))
        {
            why = fmt::format("feet contact {} (flags 0x{:X})", info.feetContact,
                              info.groundFlags);
        }
    }
    if (why.empty()) {
        if (sFeetHeldAt < 0) {
            sFeetHeldAt = ctx.ticks;
        }
        sFeetWorstSpread = std::max(sFeetWorstSpread, spread);
        if (ctx.ticks - sFeetHeldAt >= ctx.step.value("frames", 30)) {
            TwiliLog.info("[autotest] {} feet on the ground: spread {:.1f} (worst {:.1f}) bend "
                          "{:.1f}", self ? "self" : "dummy", spread, sFeetWorstSpread, bend);
            return true;
        }
        return false;
    }
    if (sFeetHeldAt >= 0) {
        ctx.fail(fmt::format("expectFeetOnGround: stopped holding after {} ticks: {}",
                             ctx.ticks - sFeetHeldAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectFeetOnGround: " + why);
    }
    return false;
}

// Until the first peer's dummy has its feet off the ground (a jump, a fall)
bool expectDummyAirborne(StepContext& ctx) {
    DummyPlayerDebugInfo info;
    fopAc_ac_c* dummy = firstPeerDummy();
    if (dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info) && !info.feetContact &&
        (info.groundFlags & kGroundFeet) == 0)
    {
        TwiliLog.info("[autotest] dummy airborne: legs {:.1f} degrees", info.legBendDeg);
        return true;
    }
    if (ctx.seconds > ctx.timeout(5.0)) {
        ctx.fail("expectDummyAirborne: the dummy's feet stayed on the ground");
    }
    return false;
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
        TwiliLog.info("[autotest] moved to {:.0f} {:.0f} {:.0f}", pos.x, pos.y, pos.z);
        return true;
    }

    if (op == "expectDummyTeleport") {
        return expectDummyTeleport(ctx);
    }

    if (op == "measureBodyHeight") {
        return measureBodyHeight(ctx);
    }

    if (op == "findSlope") {
        return findSlope(ctx);
    }

    if (op == "expectFeetOnGround") {
        return expectFeetOnGround(ctx);
    }

    if (op == "expectDummyAirborne") {
        return expectDummyAirborne(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&motionSteps);

}  // namespace
}  // namespace twili::autotest

