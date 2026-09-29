// Steps for forms, remote wolves, Midna and transformation effects; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "fx/RemoteTransformFx.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "res/Object/AlAnm.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace twili::autotest {
namespace {

constexpr uint16_t kWolfJointNum = 40;
constexpr uint16_t kHumanJointNum = 35;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

bool wantsWolf(StepContext& ctx, bool& wolf) {
    const std::string form = ctx.step.value("form", std::string("wolf"));
    if (form != "wolf" && form != "human") {
        ctx.fail(fmt::format("{}: unknown form '{}'", ctx.step.value("op", std::string{}), form));
        return false;
    }
    wolf = form == "wolf";
    return true;
}

// What `transform trace:true` saw of the local transformation, one sample per tick.
struct LocalTransformTrace {
    int lastTick = -1;
    bool seen = false;
    uint32_t startSeq = 0;
    bool toWolf = false;
    int ticksA = 0, ticksSwap = 0, ticksC = 0;
    int deadTicks = 0;  // ticks an emitter the planner expects did not exist
    std::string deadWhy;
    int16_t tevMinA = 0, tevMinC = 0, tevMaxC = 0;
    float hatMin = 1.0f;
    float endFrame = 0.0f;
};
LocalTransformTrace sLocalTf;

// Before this call's procCoMetamorphoseInit: the sample follows the execute that set the emitters.
void traceLocalTransform(StepContext& ctx, daAlink_c* link) {
    LocalTransformTrace& t = sLocalTf;
    if (!ctx.begun) {
        t = LocalTransformTrace{};
    }
    if (link == nullptr || ctx.ticks == t.lastTick) {
        return;
    }
    t.lastTick = ctx.ticks;
    RemoteTransformFx tf;
    Session::captureLocalTransformFx(tf);
    if (!tf.active()) {
        return;
    }
    if (!t.seen) {
        t.seen = true;
        t.startSeq = Session::localPoseSeq();
        t.toWolf = tf.toWolf();
    }
    const bool swap = link->getClothesChangeWaitTimer() != 0;
    const char* stage = "A";
    if (tf.postSwap()) {
        stage = "C";
        t.ticksC++;
        t.tevMinC = (std::min)(t.tevMinC, tf.tev);
        t.tevMaxC = (std::max)(t.tevMaxC, tf.tev);
    } else if (swap) {
        stage = "swap";
        t.ticksSwap++;
    } else {
        t.ticksA++;
        t.tevMinA = (std::min)(t.tevMinA, tf.tev);
    }
    if (!link->checkWolf()) {
        t.hatMin = (std::min)(t.hatMin, tf.hatScale);
    }
    t.endFrame = link->mUnderFrameCtrl[0].getFrame();
    const TransformFxPlan plan = planTransformFx(tf, link->checkWolf() != 0, swap);
    const u32 ids[2] = {link->field_0x31f8, link->field_0x31fc};
    for (int i = 0; i < 2; i++) {
        if (plan.slot[i] != TfEmitter::None && dComIfGp_particle_getEmitter(ids[i]) == nullptr) {
            if (t.deadTicks++ == 0) {
                t.deadWhy = fmt::format("slot {} (emitter {}) missing in stage {} at tick {}", i,
                    static_cast<int>(plan.slot[i]), stage, ctx.ticks);
            }
        }
    }
}

bool transform(StepContext& ctx) {
    bool wantWolf = true;
    if (!wantsWolf(ctx, wantWolf)) {
        return false;
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    const bool trace = ctx.step.value("trace", false);
    if (trace) {
        traceLocalTransform(ctx, link);
    }
    if (link != nullptr) {
        const bool isWolf = link->checkWolf() != 0;
        const bool transforming = link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
                                  link->getClothesChangeWaitTimer() != 0;
        if (isWolf == wantWolf && !transforming) {
            TwiliLog.info("[autotest] local player is {}", wantWolf ? "a wolf" : "human");
            if (!trace) {
                return true;
            }
            const LocalTransformTrace& t = sLocalTf;
            TwiliLog.info("[autotest] local transform toWolf={} A={} swap={} C={} startSeq={} "
                          "tevA={} tevC={}..{} hatMin={:.2f} endFrame={:.1f} dead={}",
                t.toWolf, t.ticksA, t.ticksSwap, t.ticksC, t.startSeq, t.tevMinA, t.tevMinC,
                t.tevMaxC, t.hatMin, t.endFrame, t.deadTicks);
            if (!t.seen || t.ticksA == 0 || t.ticksSwap == 0 || t.ticksC == 0) {
                ctx.fail(fmt::format("transform: stages seen A={} swap={} C={}", t.ticksA,
                    t.ticksSwap, t.ticksC));
            } else if (t.toWolf != wantWolf) {
                ctx.fail("transform: sent the other direction");
            } else if (t.deadTicks != 0) {
                ctx.fail(fmt::format("transform: {} ticks with an expected emitter missing, "
                                     "first: {}",
                    t.deadTicks, t.deadWhy));
            }
            return true;
        }
        // Again on every call until it takes: some procs and running events refuse it.
        if (!transforming) {
            link->procCoMetamorphoseInit();
            // Midna's transformation demo's voice (procCoMetamorphoseInit plays it only there).
            if (ctx.step.value("voice", false) && link->mProcID == daAlink_c::PROC_METAMORPHOSE) {
                link->voiceStart(Z2SE_AL_V_TRANSFORM);
                TwiliLog.info("[autotest] transform voice at pose tick {}",
                    Session::localPoseSeq() + 1);
            }
        }
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(link == nullptr ? std::string("transform: no player")
                                 : fmt::format("transform: still {} (proc {})",
                                       link->checkWolf() ? "a wolf" : "human",
                                       static_cast<int>(link->mProcID)));
    }
    return false;
}

bool expectRemoteForm(StepContext& ctx) {
    bool wolf = true;
    if (!wantsWolf(ctx, wolf)) {
        return false;
    }
    const std::string body = ctx.step.value("body", std::string(wolf ? "wolf" : "human"));
    if (body != "wolf" && body != "human" && body != "hidden") {
        ctx.fail("expectRemoteForm: unknown body '" + body + "'");
        return false;
    }
    auto& b = Session::instance();
    int matching = 0;
    std::string why = "no peer in our layer";
    for (const auto& [id, c] : b.clients()) {
        if (!peerInMyLayer(c)) continue;
        DummyPlayerDebugInfo info;
        fopAc_ac_c* dummy = b.dummyActorForClient(id);
        if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info) || !info.shellReady) {
            why = fmt::format("client {} ({}) has no dummy", id, c.name);
            continue;
        }
        const uint16_t joints = body == "wolf" ? kWolfJointNum : kHumanJointNum;
        const bool bodyOk = body == "hidden" ? info.hidden
                                             : !info.hidden && info.wolfBody == (body == "wolf") &&
                                                   info.bodyJointNum == joints;
        if ((c.transformStatus != 0) == wolf && info.remoteWolf == wolf && bodyOk) {
            matching++;
            continue;
        }
        why = fmt::format("client {} ({}): sends {}, dummy shows {} with {} joints{}", id, c.name,
            c.transformStatus != 0 ? "wolf" : "human", info.wolfBody ? "wolf" : "human",
            info.bodyJointNum, info.hidden ? ", hidden" : "");
    }
    if (matching >= ctx.step.value("count", 1)) {
        TwiliLog.info("[autotest] {} peer(s) {} with a {} dummy", matching,
            wolf ? "wolf" : "human", body);
        return true;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail("expectRemoteForm: " + why);
    }
    return false;
}

// Refusal counts of the peers' dummies when expectCleanAnims began.
std::map<uint32_t, uint32_t> sRefusedAtStart;

bool expectCleanAnims(StepContext& ctx) {
    auto& b = Session::instance();
    auto refusedOf = [&](uint32_t id, uint32_t& refused) {
        DummyPlayerDebugInfo info;
        fopAc_ac_c* dummy = b.dummyActorForClient(id);
        if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info)) {
            return false;
        }
        refused = info.refusedAnms;
        return true;
    };
    if (!ctx.begun) {
        sRefusedAtStart.clear();
        for (const auto& [id, c] : b.clients()) {
            uint32_t refused = 0;
            if (peerInMyLayer(c) && refusedOf(id, refused)) {
                sRefusedAtStart[id] = refused;
            }
        }
        if (sRefusedAtStart.empty()) {
            ctx.fail("expectCleanAnims: no peer with a dummy in our layer");
            return false;
        }
    }
    if (ctx.ticks < ctx.step.value("frames", 120)) {
        return false;
    }
    const uint32_t maxRefused = ctx.step.value("maxRefused", 0u);
    for (const auto& [id, atStart] : sRefusedAtStart) {
        uint32_t refused = 0;
        if (!refusedOf(id, refused)) {
            ctx.fail(fmt::format("expectCleanAnims: the dummy of client {} disappeared", id));
            return false;
        }
        // A dummy recreated meanwhile counts from 0 again.
        const uint32_t since = refused >= atStart ? refused - atStart : refused;
        TwiliLog.info("[autotest] refused clips: client {} {}", id, since);
        if (since > maxRefused) {
            ctx.fail(fmt::format("expectCleanAnims: the dummy of client {} refused {} clip "
                                 "requests (max {})",
                id, since, maxRefused));
            return false;
        }
    }
    return true;
}

bool setMidna(StepContext& ctx) {
    const bool ride = ctx.step.value("ride", true);
    const bool visible = ctx.step.value("visible", true);
    const bool dark = dKy_darkworld_check() != 0;
    if (!ctx.begun) {
        TwiliLog.info("[autotest] setMidna ride={} visible={} darkWorld={}", ride, visible, dark);
        if (ride && !visible && dark) {
            ctx.fail("setMidna: visible:false needs a light-world stage");
            return false;
        }
        if (ride) {
            dComIfGs_onEventBit(dSv_event_flag_c::M_067);
        } else {
            dComIfGs_offEventBit(dSv_event_flag_c::M_067);
        }
        // F_0250 stays clear: it moves Faron Woods to another layer at once.
        if (ride && visible && !dark) {
            dComIfGs_onTransformLV(3);
        } else if (ride && !visible) {
            dComIfGs_offTransformLV(3);
        }
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    const bool wolf = link != nullptr && link->checkWolf();
    const uint8_t want = !ride || !wolf ? kMidnaNone : visible ? kMidnaDrawn : kMidnaShadowOnly;
    RemoteMidnaPose pose;
    Session::captureLocalMidna(localCutsceneRunning(), pose);
    if (pose.mode == want && pose.bodyBck != dRes_ID_ALANM_BCK_MD_LEADTOWAITA_e) {
        TwiliLog.info("[autotest] local Midna mode {} clip 0x{:X}", pose.mode, pose.bodyBck);
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("setMidna: local Midna mode {} (want {}), clip 0x{:X}", pose.mode,
            want, pose.bodyBck));
    }
    return false;
}

bool isMidnaBodyClip(uint16_t id) {
    for (int i = 0; i < daMidna_c::ANM_FTALKA; i++) {
        if (daMidna_c::m_anmDataTable[i].mResID == id) return true;
    }
    return false;
}

// What of expectRemoteMidna's expectations the peer `c` and its dummy miss, empty when none.
std::string midnaMismatch(const nlohmann::json& step, uint8_t want, const Client& c,
    const DummyPlayerDebugInfo& info) {
    if (c.midna.mode != want || info.midnaMode != want) {
        return fmt::format("sends Midna mode {} (clip 0x{:X}), its dummy poses mode {}",
            c.midna.mode, c.midna.bodyBck, info.midnaMode);
    }
    if (want == kMidnaNone) {
        return {};
    }
    if (want == kMidnaApart) {
        if (!info.midnaReady || !isMidnaBodyClip(info.midnaBodyAnm) || !info.midnaShown) {
            return fmt::format("dummy Midna apart: ready {}, clip 0x{:X}, shown {}",
                info.midnaReady, info.midnaBodyAnm, info.midnaShown);
        }
        return {};
    }
    // Her root sits about 90 units from the wolf's joint WL_JNT_MD.
    if (!info.midnaReady || !info.wolfBody || !isMidnaBodyClip(info.midnaBodyAnm) ||
        info.midnaBackDist <= 0.0f || info.midnaBackDist >= 120.0f)
    {
        return fmt::format("dummy Midna ready {}, wolf body {}, clip 0x{:X}, {:.1f} from the "
                           "wolf's back joint",
            info.midnaReady, info.wolfBody, info.midnaBodyAnm, info.midnaBackDist);
    }
    if (info.midnaRefused > step.value("maxRefused", 0u)) {
        return fmt::format("dummy Midna refused {} ids", info.midnaRefused);
    }
    if (want == kMidnaDrawn && !info.midnaShown) {
        return "dummy Midna is not drawn";
    }
    std::string why;
    auto expect = [&](const char* key, int have) {
        if (why.empty() && step.contains(key) && step[key].get<int>() != have) {
            why = fmt::format("dummy Midna {} is {}", key, have);
        }
    };
    expect("upper", info.midnaUpperAnm);
    expect("hairHand", info.midnaHairHand);
    expect("leftHand", info.midnaLeftHand);
    expect("rightHand", info.midnaRightHand);
    if (why.empty() && step.contains("tired") && step["tired"].get<bool>() != info.midnaTired) {
        why = fmt::format("dummy Midna tired is {}", info.midnaTired);
    }
    return why;
}

// Tick at which the running expectRemoteMidna first matched, -1 before.
int sMidnaMatchedAt = -1;

bool expectRemoteMidna(StepContext& ctx) {
    if (!ctx.begun) {
        sMidnaMatchedAt = -1;
    }
    const std::string modeName = ctx.step.value("mode", std::string("drawn"));
    uint8_t want = kMidnaNone;
    if (modeName == "drawn") {
        want = kMidnaDrawn;
    } else if (modeName == "shadow") {
        want = kMidnaShadowOnly;
    } else if (modeName == "apart") {
        want = kMidnaApart;
    } else if (modeName != "none") {
        ctx.fail("expectRemoteMidna: unknown mode '" + modeName + "'");
        return false;
    }
    auto& b = Session::instance();
    int matching = 0;
    std::string why = "no peer in our layer";
    std::string seen;
    for (const auto& [id, c] : b.clients()) {
        if (!peerInMyLayer(c)) continue;
        DummyPlayerDebugInfo info;
        fopAc_ac_c* dummy = b.dummyActorForClient(id);
        if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info) || !info.shellReady) {
            why = fmt::format("client {} ({}) has no dummy", id, c.name);
            continue;
        }
        const std::string miss = midnaMismatch(ctx.step, want, c, info);
        if (miss.empty()) {
            matching++;
            seen = fmt::format("clip 0x{:X}, upper 0x{:X}, hair hand {}, hands {}/{}, tired {}, "
                               "{:.1f} from the wolf's back joint, {} refused",
                info.midnaBodyAnm, info.midnaUpperAnm, info.midnaHairHand, info.midnaLeftHand,
                info.midnaRightHand, info.midnaTired, info.midnaBackDist, info.midnaRefused);
            continue;
        }
        why = fmt::format("client {} ({}): {}", id, c.name, miss);
    }
    if (matching >= ctx.step.value("count", 1)) {
        if (sMidnaMatchedAt < 0) {
            sMidnaMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sMidnaMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] remote Midna {} on {} peer(s): {}", modeName, matching, seen);
        return true;
    }
    if (sMidnaMatchedAt >= 0) {
        ctx.fail(fmt::format("expectRemoteMidna: {} ticks after it matched, {}",
            ctx.ticks - sMidnaMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectRemoteMidna: " + why);
    }
    return false;
}

const Client* firstPeerDummy(uint32_t& id, DummyPlayerDebugInfo& info) {
    auto& b = Session::instance();
    for (const auto& [clientId, c] : b.clients()) {
        fopAc_ac_c* dummy = peerInMyLayer(c) ? b.dummyActorForClient(clientId) : nullptr;
        if (dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info) && info.shellReady) {
            id = clientId;
            return &c;
        }
    }
    return nullptr;
}

bool skipsStage(const nlohmann::json& step, const char* stage) {
    const auto it = step.find("skip");
    if (it == step.end() || !it->is_array()) {
        return false;
    }
    return std::find(it->begin(), it->end(), stage) != it->end();
}

// The peer and its transformation counts when the step began; the tick both showed the end.
struct RemoteTransformWatch {
    uint32_t clientId = 0;
    uint16_t wireBase = 0;
    uint16_t dummyBase = 0;
    int doneTick = -1;
};
RemoteTransformWatch sTfWatch;

// Sender ticks from the packet that started a stage to the tick the dummy showed it.
int stageLag(double shown, uint32_t seq) {
    return static_cast<int>(std::floor(shown)) - static_cast<int>(seq);
}

bool expectRemoteTransformFx(StepContext& ctx) {
    bool wolf = true;
    if (!wantsWolf(ctx, wolf)) {
        return false;
    }
    RemoteTransformWatch& w = sTfWatch;
    if (!ctx.begun) {
        w = RemoteTransformWatch{};
    }
    uint32_t id = 0;
    DummyPlayerDebugInfo info;
    const Client* c = firstPeerDummy(id, info);
    if (c != nullptr && w.clientId == 0) {
        // A transformation that reached us just before this step began still counts.
        const TransformFxTrace& t = c->transformTrace;
        const bool wireRunning = t.startSeq != 0 && t.endSeq == 0;
        w.clientId = id;
        w.wireBase = t.count - (wireRunning && t.count > 0 ? 1 : 0);
        w.dummyBase =
            info.tf.count - ((info.tf.flags & kTfActive) != 0 && info.tf.count > 0 ? 1 : 0);
    }
    if (c == nullptr || id != w.clientId) {
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail("expectRemoteTransformFx: no peer with a dummy in our layer");
        }
        return false;
    }
    const TransformFxTrace& t = c->transformTrace;
    const DummyTransformFxTrace& d = info.tf;
    const bool shown = t.count > w.wireBase && t.endSeq != 0 && d.count > w.dummyBase &&
                       d.endShown >= static_cast<double>(t.endSeq);
    if (!shown) {
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail(fmt::format("expectRemoteTransformFx: transformations sent {} (from {}), "
                                 "end seq {}, replayed {} (from {}), end shown {:.1f}",
                t.count, w.wireBase, t.endSeq, d.count, w.dummyBase, d.endShown));
        }
        return false;
    }
    // Three more ticks: the level emitters are cut on the first one they are not set again.
    if (w.doneTick < 0) {
        w.doneTick = ctx.ticks;
    }
    if (ctx.ticks - w.doneTick < 3) {
        return false;
    }

    const bool skipA = skipsStage(ctx.step, "A");
    const bool skipSwap = skipsStage(ctx.step, "swap");
    const bool skipC = skipsStage(ctx.step, "C");
    const int maxLag = ctx.step.value("maxLag", 1);
    const int lagStart = stageLag(d.startShown, t.startSeq);
    const int lagSwap = stageLag(d.swapShown, t.swapSeq);
    const int lagPost = stageLag(d.postShown, t.postSeq);
    const int lagEnd = stageLag(d.endShown, t.endSeq);
    const int flipLag = static_cast<int>(t.flipSeq) - static_cast<int>(t.clipSeq);
    const int wireA = static_cast<int>(t.swapSeq) - static_cast<int>(t.startSeq);
    const int wireSwap = static_cast<int>(t.postSeq) - static_cast<int>(t.swapSeq);
    const int wireC = static_cast<int>(t.endSeq) - static_cast<int>(t.postSeq);
    TwiliLog.info("[autotest] transform fx form={} lag start/swap/post/end={}/{}/{}/{} "
                  "shown start/clip={:.2f}/{:.2f} masks={:02X}/{:02X}/{:02X} silhouette={} "
                  "fur={}/{} tevA={} tevC={}..{} anchorErr={:.1f} src={} hidden={} wire "
                  "A/swap/C={}/{}/{} dummy A/C={}/{} endFrame={:.1f} flipLag={} seqs "
                  "start/clip/swap/post/flip/end={}/{}/{}/{}/{}/{}",
        wolf ? "wolf" : "human", lagStart, lagSwap, lagPost, lagEnd, d.startShown, d.clipShown,
        d.maskA, d.maskSwap, d.maskC, d.silhouetteTicks, d.furA, d.furC, d.tevMinA, d.tevMinC,
        d.tevMaxC, d.anchorErr, d.anchorSource, d.hiddenTicks, wireA, wireSwap, wireC, d.ticksA,
        d.ticksC, t.endFrame, flipLag, t.startSeq, t.clipSeq, t.swapSeq, t.postSeq, t.flipSeq,
        t.endSeq);

    std::string why;
    auto expect = [&why](bool ok, std::string what) {
        if (why.empty() && !ok) {
            why = std::move(what);
        }
    };
    auto lagOk = [maxLag](int lag) { return lag >= 0 && lag <= maxLag; };
    const uint8_t atow = tfEmitterBit(TfEmitter::AtowA) | tfEmitterBit(TfEmitter::AtowB);
    const uint8_t beforeSwap = wolf ? atow : tfEmitterBit(TfEmitter::WtoaB);
    const uint8_t afterSwap = wolf ? atow : tfEmitterBit(TfEmitter::WtoaA);
    expect(t.toWolf == wolf, "the sender transformed the other way");
    expect(t.startSeq != 0 && t.swapSeq > t.startSeq && t.postSeq > t.swapSeq &&
               t.endSeq > t.postSeq,
        "the stages did not reach us in order");
    expect(t.clipSeq == t.startSeq, "the start flag and the first clip came in different packets");
    if (!skipA) {
        expect(lagOk(lagStart), fmt::format("the effects started {} ticks late", lagStart));
        expect(d.clipShown != 0.0 && d.startShown == d.clipShown,
            fmt::format("the effects started at {:.2f}, the body's clip at {:.2f}", d.startShown,
                d.clipShown));
        expect(d.maskA == beforeSwap, fmt::format("emitters before the swap {:02X}", d.maskA));
        expect(d.tevMinA <= -60, fmt::format("the body only darkened to {}", d.tevMinA));
        if (ctx.step.value("fur", true) && wolf) {
            expect(d.furA, "no fur before the swap");
        }
        const int anchorSource = ctx.step.value("anchorSource", 0);
        expect(d.anchorSource == anchorSource,
            fmt::format("the anchor came from source {}", d.anchorSource));
        if (anchorSource == 0) {
            expect(d.anchorErr >= 0.0f && d.anchorErr <= ctx.step.value("maxAnchorErr", 50.0f),
                fmt::format("the anchor was {:.1f} from the sender's", d.anchorErr));
        }
    } else {
        expect(d.anchorSource == ctx.step.value("anchorSource", 0),
            fmt::format("the anchor came from source {}", d.anchorSource));
    }
    if (!skipSwap) {
        expect(lagOk(lagSwap), fmt::format("the swap showed {} ticks late", lagSwap));
        expect(d.maskSwap == beforeSwap, fmt::format("emitters in the swap {:02X}", d.maskSwap));
        expect(d.silhouetteTicks >= ctx.step.value("minSilhouette", 1) &&
                   d.silhouetteTicks <= wireSwap + 1,
            fmt::format("the silhouette drew {} ticks of a {}-tick swap", d.silhouetteTicks,
                wireSwap));
    }
    if (!skipC) {
        expect(lagOk(lagPost),
            fmt::format("the new body's effects started {} ticks late", lagPost));
        if (wolf) {
            expect(d.tevMinC <= -40, fmt::format("the wolf came in at {}", d.tevMinC));
        } else {
            expect(d.tevMaxC >= 40, fmt::format("the human came in at {}", d.tevMaxC));
        }
    }
    expect(d.maskC == afterSwap, fmt::format("emitters after the swap {:02X}", d.maskC));
    if (ctx.step.value("fur", true) && !wolf) {
        expect(d.furC, "no fur after the swap");
    }
    expect(lagOk(lagEnd), fmt::format("the effects ended {} ticks late", lagEnd));
    if (ctx.step.value("hidden", false)) {
        expect(d.hiddenTicks > 0 && d.maskA == 0 && d.maskSwap == 0,
            fmt::format("hidden {} ticks, emitters before/in the swap {:02X}/{:02X}",
                d.hiddenTicks, d.maskA, d.maskSwap));
    }
    expect(info.tfAlive == 0 && d.bodyTev == 0 && !d.silhouette && d.emitted == 0,
        fmt::format("after the end: emitters alive {:X}, tev {}, silhouette {}", info.tfAlive,
            d.bodyTev, d.silhouette));
    if (!why.empty()) {
        ctx.fail("expectRemoteTransformFx: " + why);
    }
    return true;
}

// Transformations the first peer's dummy had replayed when expectNoTransformFx began.
uint16_t sNoTfCount = 0;
uint32_t sNoTfClient = 0;

bool expectNoTransformFx(StepContext& ctx) {
    uint32_t id = 0;
    DummyPlayerDebugInfo info;
    if (firstPeerDummy(id, info) == nullptr) {
        ctx.fail("expectNoTransformFx: no peer with a dummy in our layer");
        return false;
    }
    const DummyTransformFxTrace& d = info.tf;
    if (!ctx.begun) {
        sNoTfClient = id;
        sNoTfCount = d.count;
    }
    if (id != sNoTfClient || d.count != sNoTfCount || d.emitted != 0 || d.silhouette ||
        d.bodyTev != 0 || (ctx.ticks > 3 && info.tfAlive != 0))
    {
        ctx.fail(fmt::format("expectNoTransformFx: after {} ticks: replays {} (from {}), "
                             "emitted {:02X}, alive {:X}, silhouette {}, tev {}",
            ctx.ticks, d.count, sNoTfCount, d.emitted, info.tfAlive, d.silhouette, d.bodyTev));
        return false;
    }
    if (ctx.ticks < ctx.step.value("frames", 120)) {
        return false;
    }
    TwiliLog.info("[autotest] no transformation effects for {} ticks", ctx.ticks);
    return true;
}

// Tick at which the running expectDummyTransformFx first matched, -1 before.
int sDummyTfMatchedAt = -1;

bool expectDummyTransformFx(StepContext& ctx) {
    if (!ctx.begun) {
        sDummyTfMatchedAt = -1;
    }
    uint32_t id = 0;
    DummyPlayerDebugInfo info;
    std::string why = "no peer with a dummy in our layer";
    if (firstPeerDummy(id, info) != nullptr) {
        const DummyTransformFxTrace& d = info.tf;
        const int emitters = ctx.step.value("emitters", 0);
        // The slots that hold the emitters asked for (TransformFxPlan::slot).
        const uint8_t slot0 = tfEmitterBit(TfEmitter::AtowA) | tfEmitterBit(TfEmitter::WtoaB);
        const uint8_t slot1 = tfEmitterBit(TfEmitter::AtowB) | tfEmitterBit(TfEmitter::WtoaA);
        const int wantAlive = ((emitters & slot0) != 0 ? 1 : 0) | ((emitters & slot1) != 0 ? 2 : 0);
        why.clear();
        if (d.emitted != emitters || (info.tfAlive & wantAlive) != wantAlive) {
            why = fmt::format("emitted {:02X}, alive {:X}", d.emitted, info.tfAlive);
        } else if (ctx.step.contains("tev") && ctx.step["tev"].get<int>() != d.bodyTev) {
            why = fmt::format("tev {}", d.bodyTev);
        } else if (ctx.step.contains("silhouette") &&
                   ctx.step["silhouette"].get<bool>() != d.silhouette)
        {
            why = fmt::format("silhouette {}", d.silhouette);
        } else if (ctx.step.contains("anchorSource") &&
                   ctx.step["anchorSource"].get<int>() != d.anchorSource)
        {
            why = fmt::format("anchor source {}", d.anchorSource);
        }
        if (why.empty()) {
            if (sDummyTfMatchedAt < 0) {
                sDummyTfMatchedAt = ctx.ticks;
            }
            if (ctx.ticks - sDummyTfMatchedAt < ctx.step.value("frames", 0)) {
                return false;
            }
            TwiliLog.info("[autotest] dummy transformation effects {:02X} tev {} silhouette {} "
                          "anchor source {} hat {:.2f}",
                d.emitted, d.bodyTev, d.silhouette, d.anchorSource, d.hatScale);
            return true;
        }
    }
    if (sDummyTfMatchedAt >= 0) {
        ctx.fail(fmt::format("expectDummyTransformFx: {} ticks after it matched, {}",
            ctx.ticks - sDummyTfMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectDummyTransformFx: " + why);
    }
    return false;
}

bool waitRemoteTransformPhase(StepContext& ctx) {
    const std::string phase = ctx.step.value("phase", std::string("C"));
    if (phase != "A" && phase != "swap" && phase != "C") {
        ctx.fail("waitRemoteTransformPhase: unknown phase '" + phase + "'");
        return false;
    }
    uint32_t id = 0;
    DummyPlayerDebugInfo info;
    const Client* c = firstPeerDummy(id, info);
    if (c != nullptr && c->transformFx.active()) {
        const bool post = c->transformFx.postSwap();
        const std::string now = post ? "C" : c->modelSwap ? "swap" : "A";
        if (now == phase) {
            TwiliLog.info("[autotest] remote transformation in stage {}", phase);
            return true;
        }
    }
    if (ctx.seconds > ctx.timeout(60.0)) {
        ctx.fail("waitRemoteTransformPhase: the peer never sent stage " + phase);
    }
    return false;
}

// The first peer's dummy in our layer, or nullptr.
fopAc_ac_c* firstPeerDummy(const Client** peer = nullptr) {
    auto& b = Session::instance();
    for (const auto& [id, c] : b.clients()) {
        if (!peerInMyLayer(c)) continue;
        if (fopAc_ac_c* dummy = b.dummyActorForClient(id)) {
            if (peer != nullptr) *peer = &c;
            return dummy;
        }
    }
    return nullptr;
}

// Counters markRemoteMidna took from the first peer's dummy.
struct MidnaMark {
    bool valid = false;
    uint32_t sfx = 0;
    uint32_t apartTicks = 0;
    uint32_t shownTicks = 0;
};
MidnaMark sMidnaMark;

bool markRemoteMidna(StepContext& ctx) {
    DummyPlayerDebugInfo info;
    fopAc_ac_c* dummy = firstPeerDummy();
    if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info) || !info.shellReady) {
        if (ctx.seconds > ctx.timeout(20.0)) ctx.fail("markRemoteMidna: no peer dummy");
        return false;
    }
    sMidnaMark = {true, info.midnaSfx, info.midnaApartTicks, info.midnaShownTicks};
    TwiliLog.info("[autotest] remote Midna marked: {} sounds, {} ticks apart", info.midnaSfx,
        info.midnaApartTicks);
    return true;
}

// Midna's sounds heard and her time off the wolf's back since markRemoteMidna.
bool expectRemoteMidnaSince(StepContext& ctx) {
    if (!sMidnaMark.valid) {
        ctx.fail("expectRemoteMidnaSince: no markRemoteMidna before");
        return false;
    }
    DummyPlayerDebugInfo info;
    fopAc_ac_c* dummy = firstPeerDummy();
    const bool have = dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info) && info.shellReady;
    const uint32_t sfx = have ? info.midnaSfx - sMidnaMark.sfx : 0;
    const uint32_t apart = have ? info.midnaApartTicks - sMidnaMark.apartTicks : 0;
    const uint32_t shown = have ? info.midnaShownTicks - sMidnaMark.shownTicks : 0;
    if (have && sfx >= ctx.step.value("minSfx", 0u) &&
        apart >= ctx.step.value("minApartTicks", 0u) &&
        shown >= ctx.step.value("minShownTicks", 0u))
    {
        TwiliLog.info("[autotest] remote Midna since the mark: {} sounds, {} ticks apart, {} "
                      "ticks shown",
            sfx, apart, shown);
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("expectRemoteMidnaSince: {} sounds (want {}), {} ticks apart "
                             "(want {}), {} ticks shown (want {}){}",
            sfx, ctx.step.value("minSfx", 0u), apart, ctx.step.value("minApartTicks", 0u), shown,
            ctx.step.value("minShownTicks", 0u), have ? "" : ", no peer dummy"));
    }
    return false;
}

// A PLAYER_SFX played on the dummy's playout clock at the pose tick it was made in.
bool expectRemoteSfxTiming(StepContext& ctx) {
    const uint32_t sound = ctx.step.value("sound", 0u);
    const double maxLag = ctx.step.value("maxLag", 1.0);
    const Client* peer = nullptr;
    DummyPlayerDebugInfo info;
    fopAc_ac_c* dummy = firstPeerDummy(&peer);
    if (dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info)) {
        std::vector<DummySfxPlayed> recent(std::begin(info.voiceRecent), std::end(info.voiceRecent));
        recent.insert(recent.end(), std::begin(info.sfxRecent), std::end(info.sfxRecent));
        for (const DummySfxPlayed& p : recent) {
            if (p.id != sound || p.seq == 0) continue;
            const double lag = p.shown - p.seq;
            std::string why;
            if (lag < 0.0 || lag > maxLag) {
                why = fmt::format("played at tick {:.2f}, {:.2f} after its tick {}", p.shown, lag,
                    p.seq);
            }
            double tfLag = 0.0;
            if (why.empty() && ctx.step.value("transform", false)) {
                const TransformFxTrace& t = peer->transformTrace;
                tfLag = p.shown - info.tf.startShown;
                const int32_t wireLag = static_cast<int32_t>(p.seq - t.startSeq);
                if (t.startSeq == 0 || info.tf.startShown <= 0.0 || std::abs(wireLag) > 1 ||
                    std::fabs(tfLag) > maxLag + 1.0)
                {
                    why = fmt::format("voice tick {} vs the transformation's {} (wire), played "
                                      "{:.2f} ticks from its first shown tick {:.2f}",
                        p.seq, t.startSeq, tfLag, info.tf.startShown);
                }
            }
            if (!why.empty()) {
                ctx.fail("expectRemoteSfxTiming: " + why);
                return false;
            }
            TwiliLog.info("[autotest] sound 0x{:X} made at tick {} played at shown tick {:.2f} "
                          "(lag {:.2f}, {:.2f} from the transformation's start), {} played, {} "
                          "dropped",
                sound, p.seq, p.shown, lag, tfLag, info.sfxPlayed, info.sfxDropped);
            return true;
        }
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(fmt::format("expectRemoteSfxTiming: sound 0x{:X} never played on the clock "
                             "({} played, {} dropped)",
            sound, info.sfxPlayed, info.sfxDropped));
    }
    return false;
}

std::optional<bool> wolfRemoteSteps(const std::string& op, StepContext& ctx) {
    if (op == "setForm") {
        bool wolf = true;
        if (!wantsWolf(ctx, wolf)) {
            return false;
        }
        // Without M_077 the form comes from the areas' twilight state.
        dComIfGs_onEventBit(dSv_event_flag_c::M_077);
        dComIfGs_setTransformStatus(wolf ? 1 : 0);
        return true;
    }
    if (op == "transform") {
        return transform(ctx);
    }
    if (op == "expectLocalForm") {
        bool wolf = true;
        if (!wantsWolf(ctx, wolf)) {
            return false;
        }
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr || (link->checkWolf() != 0) != wolf) {
            ctx.fail(fmt::format("expectLocalForm: the local player is {}",
                link == nullptr ? "missing" : link->checkWolf() ? "a wolf" : "human"));
            return false;
        }
        return true;
    }
    if (op == "expectRemoteForm") {
        return expectRemoteForm(ctx);
    }
    if (op == "expectCleanAnims") {
        return expectCleanAnims(ctx);
    }
    if (op == "setMidna") {
        return setMidna(ctx);
    }
    if (op == "expectRemoteMidna") {
        return expectRemoteMidna(ctx);
    }
    if (op == "midnaSfx") {
        daMidna_c* midna = daPy_py_c::getMidnaActor();
        if (midna == nullptr) {
            ctx.fail("midnaSfx: no Midna");
            return false;
        }
        const uint32_t sound = ctx.step.value("sound", 0u);
        if (ctx.step.value("voice", true)) {
            midna->mSound.startCreatureVoice(sound, 0);
        } else {
            midna->mSound.startCreatureSound(sound, 0, 0);
        }
        TwiliLog.info("[autotest] Midna sound 0x{:X} at pose tick {}", sound,
            Session::localPoseSeq() + 1);
        return true;
    }
    if (op == "markRemoteMidna") {
        return markRemoteMidna(ctx);
    }
    if (op == "expectRemoteMidnaSince") {
        return expectRemoteMidnaSince(ctx);
    }
    if (op == "expectRemoteSfxTiming") {
        return expectRemoteSfxTiming(ctx);
    }
    if (op == "transformFxSelfTest") {
        std::string why;
        if (!runTransformFxSelfTest(why)) {
            ctx.fail("transformFxSelfTest: " + why);
            return false;
        }
        TwiliLog.info("[autotest] transformFxSelfTest passed");
        return true;
    }
    if (op == "expectRemoteTransformFx") {
        return expectRemoteTransformFx(ctx);
    }
    if (op == "expectNoTransformFx") {
        return expectNoTransformFx(ctx);
    }
    if (op == "expectDummyTransformFx") {
        return expectDummyTransformFx(ctx);
    }
    if (op == "waitRemoteTransformPhase") {
        return waitRemoteTransformPhase(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&wolfRemoteSteps);

}  // namespace
}  // namespace twili::autotest
