// Steps for weapons and items out in the world (fx/ItemFx.cpp on the sender, actors/DummyItemFx.cpp
// and DummyPlayer.cpp on the dummy). Kinds are "arrow", "boomerang", "bomb", "spinner" and
// "crodBall"; event types "explode", "hitMark", "sound", "water" and "particle".
//
// spawnLocalItem  kind ("arrow", "bombArrow", "seed", "bomb", "waterBomb", "bombling"), count (1),
//                 forward (150), up (40), pitch (degrees down, 0), aimAtDummy, atDummy,
//                 timeoutSec (5)
//     Our real item actors in our player's layer, without the aim. Arrows and seeds fly from our
//     chest along our facing, or at the first peer's dummy with aimAtDummy; bombs appear
//     `forward` ahead, or at the dummy with atDummy. Done once every arrow flies.
// explodeAtDummy
//     A bomb arrow's explosion (setBombArrowExplode) beside the first peer's dummy.
// injectItemFx  slots ([{kind, sub, state, flags, forward (100), up (0), aux, fuse}]),
//               events ([{type, arg, forward (100), up (0)}]), hook ({mode (3), sub,
//               forward (300), up (100)}), ball ({mode (6), links (30), forward (250), up (0)}),
//               packets (60)
//     Our next `packets` updates show these; `fuse` sets a bomb's aux that many ticks ahead.
//     The events go out once, with the next update.
// setOil  value (21600)
// expectLocalItemFx  objects ({kind: n}), events ({type: n}), hookOutTicks, ironBallTicks,
//                    levelSfxTicks, minHookDist, timeoutSec (10)
//     Until our capture's totals since the game started reach these.
// markRemoteItemFx
//     Remembers the first peer dummy's counters for expectRemoteItemFx.
// expectRemoteItemFx  minDrawn / maxDrawn ({kind: n}), minSeen ({kind: n} since the mark),
//                     explosions, hitMarks, sounds, splashes, particles (since the mark),
//                     maxExplosions, tornado, boomCharge, minLights, maxLights, minEmitters,
//                     legacyProjectile, maxHeapUsed, hookChain, minTipDist, minIronBallMode,
//                     maxIronBallMode, minIronBallDist, lanternFlame, minLanternGlow,
//                     maxLanternGlow, levelSfx; frames (0), timeoutSec (20)
//     Until the first peer's dummy shows every field given, then for `frames` more ticks.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/ItemFx.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/d_bomb.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"

#include <fmt/format.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;

constexpr const char* kKindNames[kItemFxKindCount] = {"",     "arrow",   "boomerang",
                                                      "bomb", "spinner", "crodBall"};
constexpr const char* kEventNames[kItemFxEvTypeCount] = {"",      "explode", "hitMark",
                                                         "sound", "water",   "particle"};

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

fopAc_ac_c* firstPeerDummy() {
    auto& session = Session::instance();
    for (const auto& [id, c] : session.clients()) {
        if (peerInMyLayer(c)) {
            if (fopAc_ac_c* dummy = session.dummyActorForClient(id)) return dummy;
        }
    }
    return nullptr;
}

int kindIndex(const std::string& name) {
    for (int i = 1; i < kItemFxKindCount; i++) {
        if (name == kKindNames[i]) return i;
    }
    return -1;
}

int eventIndex(const std::string& name) {
    for (int i = 1; i < kItemFxEvTypeCount; i++) {
        if (name == kEventNames[i]) return i;
    }
    return -1;
}

// `forward` units along our facing and `up` above our feet.
cXyz inFront(const daAlink_c* link, float forward, float up) {
    const s16 yaw = link->shape_angle.y;
    return cXyz(link->current.pos.x + cM_ssin(yaw) * forward, link->current.pos.y + up,
                link->current.pos.z + cM_scos(yaw) * forward);
}

// Steps run between frames in another layer; an arrow made there is drawn twice a frame.
class ScopedPlayerLayer {
public:
    explicit ScopedPlayerLayer(daAlink_c* link) : mSaved(fpcLy_CurrentLayer()) {
        fpcLy_SetCurrentLayer(link->layer_tag.layer);
    }
    ~ScopedPlayerLayer() { fpcLy_SetCurrentLayer(mSaved); }

private:
    layer_class* mSaved;
};

// `up` above the dummy's feet and 40 units toward us, so the blast pushes it our way.
cXyz besideDummy(const fopAc_ac_c* dummy, const daAlink_c* link, float up) {
    cXyz toUs = link->current.pos - dummy->current.pos;
    toUs.y = 0.0f;
    if (toUs.abs() > 1.0f) {
        toUs.normalizeZP();
    }
    return cXyz(dummy->current.pos.x + toUs.x * 40.0f, dummy->current.pos.y + up,
                dummy->current.pos.z + toUs.z * 40.0f);
}

// The arrows and seeds the running spawnLocalItem still has to point.
struct PendingShot {
    fpc_ProcID id;
    int index;
    bool aimed;
    bool seen = false;  // created: fopAcM_create makes it in the next process pass
};
std::vector<PendingShot> sShots;

// Its start, its speed (the magnitude arrowShooting gave it) and the angles procMove reads.
void aimShot(fopAc_ac_c* arrow, const cXyz& start, const cXyz& dir) {
    const f32 speed = arrow->speed.abs();
    arrow->current.pos = start;
    arrow->old.pos = start;
    arrow->speed = dir * speed;
    arrow->shape_angle.x = dir.atan2sY_XZ();
    arrow->shape_angle.y = dir.atan2sX_Z();
    arrow->current.angle.x = -arrow->shape_angle.x;
    arrow->current.angle.y = arrow->shape_angle.y;
}

bool spawnLocalItem(StepContext& ctx) {
    const json& step = ctx.step;
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("spawnLocalItem: no player");
        return false;
    }
    const std::string kind = step.value("kind", std::string("arrow"));
    const int count = step.value("count", 1);
    const bool aimAtDummy = step.value("aimAtDummy", false);
    const bool needDummy = aimAtDummy || step.value("atDummy", false);
    fopAc_ac_c* dummy = needDummy ? firstPeerDummy() : nullptr;
    if (needDummy && dummy == nullptr) {
        ctx.fail("spawnLocalItem: no dummy for a peer in our layer");
        return false;
    }
    const bool shot = kind == "arrow" || kind == "bombArrow" || kind == "seed";
    if (!ctx.begun) {
        ScopedPlayerLayer layer(link);
        sShots.clear();
        for (int i = 0; i < count; i++) {
            if (shot) {
                fopAc_ac_c* a = nullptr;
                if (kind == "seed") {
                    cXyz pos = inFront(link, 30.0f, 100.0f);
                    a = daArrow_c::makeSlingStone(link, &pos);
                } else {
                    // daArrow_c::makeArrow's actor.
                    const fpc_ProcID id =
                        fopAcM_create(fpcNm_ARROW_e, (kind == "bombArrow" ? 1 : 0) << 8,
                                      &link->current.pos, fopAcM_GetRoomNo(link), NULL, NULL, -1);
                    if (id == fpcM_ERROR_PROCESS_ID_e) {
                        ctx.fail("spawnLocalItem: the arrow was not created");
                        return false;
                    }
                    sShots.push_back({id, i, false});
                    continue;
                }
                if (a == nullptr) {
                    ctx.fail("spawnLocalItem: the arrow was not created");
                    return false;
                }
                sShots.push_back({fopAcM_GetID(a), i, false, true});
                continue;
            }
            cXyz pos = dummy != nullptr
                           ? besideDummy(dummy, link, 40.0f)
                           : inFront(link, step.value("forward", 150.0f) + i * 40.0f,
                                     step.value("up", 40.0f));
            fopAc_ac_c* bomb = kind == "waterBomb" ? dBomb_c::createWaterBombPlayer(&pos)
                               : kind == "bombling" ? dBomb_c::createInsectBombPlayer(&pos)
                                                    : dBomb_c::createNormalBombPlayer(&pos);
            if (bomb == nullptr) {
                ctx.fail("spawnLocalItem: the bomb was not created");
                return false;
            }
        }
        TwiliLog.info("[autotest] spawned {} x{}", kind, count);
        return !shot;
    }

    bool allAimed = true;
    for (PendingShot& s : sShots) {
        fopAc_ac_c* a = s.aimed ? nullptr : fopAcM_SearchByID(s.id);
        if (!s.aimed && a == nullptr && !s.seen) {
            allAimed = false;  // not created yet
            continue;
        }
        if (s.aimed || a == nullptr) {
            s.aimed = true;
            continue;
        }
        s.seen = true;
        const u32 param = fopAcM_GetParam(a);
        if (param == 0) {
            // procWait holds it in the hand until it is shot.
            static_cast<daArrow_c*>(a)->setShoot();
            allAimed = false;
            continue;
        }
        if (param != 1 && param != 2) {
            // It hit something on its first tick, flying from our empty hand.
            s.aimed = true;
            continue;
        }
        cXyz start = inFront(link, 40.0f, 110.0f);
        cXyz dir;
        if (dummy != nullptr) {
            const cXyz target(dummy->current.pos.x, dummy->current.pos.y + 90.0f,
                              dummy->current.pos.z);
            dir = target - start;
        } else {
            const s16 yaw = static_cast<s16>(link->shape_angle.y + (s.index - count / 2) * 0x600);
            const s16 pitch = cM_deg2s(step.value("pitch", 0.0f));
            dir.set(cM_ssin(yaw) * cM_scos(pitch), -cM_ssin(pitch), cM_scos(yaw) * cM_scos(pitch));
        }
        if (dir.abs() < 1.0f) {
            dir = cXyz(0.0f, 0.0f, 1.0f);
        }
        dir.normalizeZP();
        aimShot(a, start, dir);
        s.aimed = true;
    }
    if (allAimed) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(5.0)) {
        ctx.fail("spawnLocalItem: the arrows never left the hand");
    }
    return false;
}

bool injectItemFx(StepContext& ctx) {
    const json& step = ctx.step;
    const daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("injectItemFx: no player");
        return false;
    }
    RemoteItemFx slots;
    int i = 0;
    for (const json& s : step.value("slots", json::array())) {
        if (i >= kItemFxSlots) break;
        ItemFxSlot& out = slots.slots[i++];
        const int kind = kindIndex(s.value("kind", std::string("arrow")));
        if (kind < 0) {
            ctx.fail("injectItemFx: unknown kind");
            return false;
        }
        out.kind = static_cast<uint8_t>(kind);
        out.sub = static_cast<uint8_t>(s.value("sub", 0));
        out.state = static_cast<uint8_t>(s.value("state", 1));
        out.flags = static_cast<uint8_t>(s.value("flags", 0));
        out.id = static_cast<uint16_t>(s.value("id", 0xF000 + i));
        const cXyz pos = inFront(link, s.value("forward", 100.0f), s.value("up", 0.0f));
        out.pos[0] = pos.x;
        out.pos[1] = pos.y;
        out.pos[2] = pos.z;
        out.ang[1] = link->shape_angle.y;
        out.aux = static_cast<uint16_t>(s.value("aux", 256));
        if (s.contains("fuse")) {
            out.aux = static_cast<uint16_t>(Session::localPoseSeq() + s.value("fuse", 90));
        }
    }
    std::vector<ItemFxEvent> events;
    for (const json& e : step.value("events", json::array())) {
        const int type = eventIndex(e.value("type", std::string("sound")));
        if (type < 0) {
            ctx.fail("injectItemFx: unknown event type");
            return false;
        }
        ItemFxEvent ev;
        ev.type = static_cast<uint8_t>(type);
        ev.arg = e.value("arg", 0u);
        const cXyz pos = inFront(link, e.value("forward", 100.0f), e.value("up", 0.0f));
        ev.pos[0] = pos.x;
        ev.pos[1] = pos.y;
        ev.pos[2] = pos.z;
        ev.rot[1] = link->shape_angle.y;
        events.push_back(ev);
    }
    if (step.contains("hook")) {
        const json& h = step["hook"];
        RemoteHookshot& hk = slots.hk;
        hk.mode = static_cast<uint8_t>(h.value("mode", static_cast<int>(kItemFxHookShoot)));
        hk.sub = static_cast<uint8_t>(h.value("sub", 0));
        const cXyz tip = inFront(link, h.value("forward", 300.0f), h.value("up", 100.0f));
        hk.tip[0] = tip.x;
        hk.tip[1] = tip.y;
        hk.tip[2] = tip.z;
        hk.tipAng[1] = link->shape_angle.y;
        hk.tipFrame = 14.0f;
    }
    if (step.contains("ball")) {
        const json& b = step["ball"];
        RemoteIronBall& bc = slots.bc;
        bc.mode = static_cast<uint8_t>(b.value("mode", 6));
        bc.links = static_cast<int16_t>(b.value("links", 30));
        const cXyz ball = inFront(link, b.value("forward", 250.0f), b.value("up", 0.0f));
        bc.ball[0] = ball.x;
        bc.ball[1] = ball.y;
        bc.ball[2] = ball.z;
        bc.ballAng[1] = link->shape_angle.y;
    }
    itemfx::injectForTest(slots, step.value("packets", 60), events.data(),
                          static_cast<int>(events.size()));
    return true;
}

// Checks `want` ({kind: n}) against per-kind values.
std::string perKindMismatch(const json& want, const char* key,
                            const uint32_t (&have)[kItemFxKindCount], bool atLeast) {
    for (auto it = want.begin(); it != want.end(); ++it) {
        const int k = kindIndex(it.key());
        if (k < 0) return fmt::format("{}: unknown kind {}", key, it.key());
        const uint32_t n = it.value().get<uint32_t>();
        if (atLeast ? have[k] < n : have[k] > n) {
            return fmt::format("{} {} is {}", key, it.key(), have[k]);
        }
    }
    return {};
}

DummyItemFxDebug sMark;
int sItemFxMatchedAt = -1;

std::string remoteMismatch(const json& step, const DummyPlayerDebugInfo& info) {
    const DummyItemFxDebug& fx = info.itemFx;
    uint32_t drawn[kItemFxKindCount] = {};
    uint32_t seen[kItemFxKindCount] = {};
    for (int k = 0; k < kItemFxKindCount; k++) {
        drawn[k] = fx.drawn[k];
        seen[k] = fx.seen[k] - sMark.seen[k];
    }
    std::string why;
    if (step.contains("minDrawn")) why = perKindMismatch(step["minDrawn"], "drawn", drawn, true);
    if (why.empty() && step.contains("maxDrawn")) {
        why = perKindMismatch(step["maxDrawn"], "drawn", drawn, false);
    }
    if (why.empty() && step.contains("minSeen")) {
        why = perKindMismatch(step["minSeen"], "seen", seen, true);
    }
    auto atLeast = [&](const char* key, uint32_t have) {
        if (why.empty() && step.contains(key) && have < step[key].get<uint32_t>()) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    atLeast("explosions", fx.explosions - sMark.explosions);
    atLeast("hitMarks", fx.hitMarks - sMark.hitMarks);
    atLeast("sounds", fx.sounds - sMark.sounds);
    atLeast("splashes", fx.splashes - sMark.splashes);
    atLeast("particles", fx.particles - sMark.particles);
    atLeast("minLights", fx.lights);
    atLeast("minEmitters", fx.emitters);
    atLeast("minIronBallMode", info.ironBallMode);
    auto atLeastF = [&](const char* key, float have) {
        if (why.empty() && step.contains(key) && have < step[key].get<float>()) {
            why = fmt::format("{} is {:.1f}", key, have);
        }
    };
    atLeastF("minTipDist", info.hookTipDist);
    atLeastF("minIronBallDist", info.ironBallDist);
    atLeastF("minLanternGlow", info.lanternGlow);
    if (why.empty() && step.contains("levelSfx")) {
        const bool grew = fx.levelSfxTicks > sMark.levelSfxTicks;
        if (grew != step["levelSfx"].get<bool>()) {
            why = fmt::format("level sounds started {} ticks since the mark (last 0x{:X})",
                              fx.levelSfxTicks - sMark.levelSfxTicks, fx.lastLevelSfx);
        }
    }
    auto atMost = [&](const char* key, uint32_t have) {
        if (why.empty() && step.contains(key) && have > step[key].get<uint32_t>()) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    atMost("maxExplosions", fx.explosions - sMark.explosions);
    atMost("maxLights", fx.lights);
    atMost("maxHeapUsed", info.heapUsed);
    atMost("maxIronBallMode", info.ironBallMode);
    if (why.empty() && step.contains("maxLanternGlow") &&
        info.lanternGlow > step["maxLanternGlow"].get<float>())
    {
        why = fmt::format("maxLanternGlow is {:.2f}", info.lanternGlow);
    }
    auto same = [&](const char* key, bool have) {
        if (why.empty() && step.contains(key) && step[key].get<bool>() != have) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    same("tornado", fx.tornado);
    same("boomCharge", fx.boomCharge);
    same("legacyProjectile", info.legacyProjectile);
    same("hookChain", info.hookChain);
    same("lanternFlame", info.lanternFlame);
    return why;
}

bool expectRemoteItemFx(StepContext& ctx) {
    if (!ctx.begun) {
        sItemFxMatchedAt = -1;
    }
    fopAc_ac_c* dummy = firstPeerDummy();
    DummyPlayerDebugInfo info;
    const bool found = dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info);
    const std::string why = found ? remoteMismatch(ctx.step, info) : "no peer dummy in our layer";
    if (why.empty()) {
        if (sItemFxMatchedAt < 0) {
            sItemFxMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sItemFxMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        const DummyItemFxDebug& fx = info.itemFx;
        TwiliLog.info("[autotest] remote item fx: drawn {}/{}/{}/{}/{} seen {}/{}/{}/{}/{} "
                      "explosions {} hit marks {} sounds {} splashes {} particles {} skipped {} "
                      "emitters {} lights {} tornado {} charge {} hook {} ({:.0f}) ball {} "
                      "({:.0f}) lantern {} ({:.2f}) level sfx {} heap 0x{:X}",
                      fx.drawn[1], fx.drawn[2], fx.drawn[3], fx.drawn[4], fx.drawn[5], fx.seen[1],
                      fx.seen[2], fx.seen[3], fx.seen[4], fx.seen[5], fx.explosions, fx.hitMarks,
                      fx.sounds, fx.splashes, fx.particles, fx.eventsSkipped, fx.emitters,
                      fx.lights, fx.tornado, fx.boomCharge, info.hookChain, info.hookTipDist,
                      info.ironBallMode, info.ironBallDist, info.lanternFlame, info.lanternGlow,
                      fx.levelSfxTicks, info.heapUsed);
        return true;
    }
    if (sItemFxMatchedAt >= 0) {
        ctx.fail(fmt::format("expectRemoteItemFx: {} ticks after it matched, {}",
                             ctx.ticks - sItemFxMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectRemoteItemFx: " + why);
    }
    return false;
}

bool expectLocalItemFx(StepContext& ctx) {
    const json& step = ctx.step;
    const itemfx::SenderStats& st = itemfx::senderStats();
    std::string why;
    const json objects = step.value("objects", json::object());
    for (auto it = objects.begin(); why.empty() && it != objects.end(); ++it) {
        const int k = kindIndex(it.key());
        if (k < 0 || st.objects[k] < it.value().get<uint32_t>()) {
            why = fmt::format("objects {} is {}", it.key(), k < 0 ? 0 : st.objects[k]);
        }
    }
    const json events = step.value("events", json::object());
    for (auto it = events.begin(); why.empty() && it != events.end(); ++it) {
        const int t = eventIndex(it.key());
        if (t < 0 || st.events[t] < it.value().get<uint32_t>()) {
            why = fmt::format("events {} is {}", it.key(), t < 0 ? 0 : st.events[t]);
        }
    }
    auto atLeast = [&](const char* key, uint32_t have) {
        if (why.empty() && step.contains(key) && have < step[key].get<uint32_t>()) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    atLeast("hookOutTicks", st.hookOutTicks);
    atLeast("ironBallTicks", st.ironBallTicks);
    atLeast("levelSfxTicks", st.levelSfxTicks);
    if (why.empty() && step.contains("minHookDist") &&
        st.hookMaxDist < step["minHookDist"].get<float>())
    {
        why = fmt::format("minHookDist is {:.1f}", st.hookMaxDist);
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] local item fx: objects {}/{}/{}/{}/{} events {}/{}/{}/{}/{} "
                      "evicted {} hook ticks {} (max {:.0f}) ball ticks {} level sfx ticks {} "
                      "(dropped {})",
                      st.objects[1], st.objects[2], st.objects[3], st.objects[4], st.objects[5],
                      st.events[1], st.events[2], st.events[3], st.events[4], st.events[5],
                      st.evicted, st.hookOutTicks, st.hookMaxDist, st.ironBallTicks,
                      st.levelSfxTicks, st.levelSfxDropped);
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format("expectLocalItemFx: {} (events {}/{}/{}/{}/{}, hook max {:.0f})", why,
                             st.events[1], st.events[2], st.events[3], st.events[4], st.events[5],
                             st.hookMaxDist));
    }
    return false;
}

std::optional<bool> itemFxSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "spawnLocalItem") {
        return spawnLocalItem(ctx);
    }

    if (op == "explodeAtDummy") {
        fopAc_ac_c* dummy = firstPeerDummy();
        if (dummy == nullptr) {
            ctx.fail("explodeAtDummy: no dummy for a peer in our layer");
            return false;
        }
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr) {
            ctx.fail("explodeAtDummy: no player");
            return false;
        }
        cXyz pos = besideDummy(dummy, link, 60.0f);
        // daArrow_c::setBombArrowExplode, hook included.
        {
            ScopedPlayerLayer layer(link);
            dBomb_c::createNormalBombExplode(&pos);
        }
        itemfx::noteBombArrowExplode(pos, false);
        TwiliLog.info("[autotest] bomb arrow blast at the dummy");
        return true;
    }

    if (op == "injectItemFx") {
        return injectItemFx(ctx);
    }

    if (op == "setOil") {
        const u16 oil = static_cast<u16>(step.value("value", 21600));
        if (dComIfGs_getMaxOil() < oil) {
            dComIfGs_setMaxOil(oil);
        }
        dComIfGs_setOil(oil);
        return true;
    }

    if (op == "expectLocalItemFx") {
        return expectLocalItemFx(ctx);
    }

    if (op == "markRemoteItemFx") {
        fopAc_ac_c* dummy = firstPeerDummy();
        DummyPlayerDebugInfo info;
        if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info)) {
            ctx.fail("markRemoteItemFx: no dummy for a peer in our layer");
            return false;
        }
        sMark = info.itemFx;
        return true;
    }

    if (op == "expectRemoteItemFx") {
        return expectRemoteItemFx(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&itemFxSteps);

}  // namespace
}  // namespace twili::autotest
