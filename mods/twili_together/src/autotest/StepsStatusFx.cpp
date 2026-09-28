// Steps for status effects on remote players; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/StatusFx.hpp"

#include "JSystem/JParticle/JPAEmitterManager.h"
#include "JSystem/JParticle/JPAResourceManager.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_particle.h"
#include "d/d_particle_name.h"

#include <fmt/format.h>

#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

// Whether this stage's scene particle pack holds `id`.
bool scenePackHas(u16 id) {
    JPAEmitterManager* manager = dPa_control_c::getEmitterManager();
    JPAResourceManager* scene =
        manager != nullptr ? manager->getResourceManager(static_cast<u8>(1)) : nullptr;
    return scene != nullptr && scene->getResource(id) != nullptr;
}

bool iceBlockAvailable() {
    return scenePackHas(dPa_RM(ID_ZI_S_LK_FREEZ_A));
}

// setElecDamageEffect's first emitter.
bool sparksAvailable() {
    const char* stage = dComIfGp_getStartStageName();
    const bool throneRoom = stage != nullptr && std::strcmp(stage, "D_MN09A") == 0;
    return scenePackHas(dPa_RM(throneRoom ? ID_ZI_S_LK_BIRIBIRIC_A : ID_ZI_S_LK_BIRIBIRIA_A));
}

bool forceStatus(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string kind = step.value("kind", std::string{});
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        ctx.fail("forceStatus: no player");
        return false;
    }
    const bool wolf = link->checkWolf() != 0;

    if (kind == "freeze") {
        if (!ctx.begun) {
            if (wolf) {
                link->procWolfDamageInit(nullptr);
            } else {
                link->procDamageInit(nullptr, FALSE);
            }
        }
        if (link->checkFreezeDamage()) {
            TwiliLog.info("[autotest] local player frozen (proc 0x{:X})",
                          static_cast<int>(link->mProcID));
            return true;
        }
    } else if (kind == "burn") {
        if (!ctx.begun) {
            link->initFirePointDamageEffectAll();
        }
        for (const auto& e : link->field_0x32d8) {
            if (e.field_0x0 != 0 && e.field_0x4 != 0) {
                TwiliLog.info("[autotest] local player on fire");
                return true;
            }
        }
    } else if (kind == "shieldBurn") {
        // setItemMatrix: pass number 15 is off exactly while a shield is in hand.
        if (!daPy_py_c::checkWoodShieldEquip() || link->field_0x2e44.checkPassNum(15) || wolf) {
            ctx.fail(fmt::format("forceStatus shieldBurn: no wooden shield in hand (shield 0x{:X})",
                                 dComIfGs_getSelectEquipShield()));
            return false;
        }
        link->field_0x2fcb = static_cast<u8>(step.value("value", 120));
        TwiliLog.info("[autotest] local shield burning ({})", link->field_0x2fcb);
        return true;
    } else if (kind == "douse") {
        link->clearWoodShieldBurnEffect();
        return true;
    } else if (kind == "elec") {
        if (link->mProcID != daAlink_c::PROC_ELEC_DAMAGE) {
            link->procCoElecDamageInit(nullptr, nullptr, 0);
        }
        if (link->mProcID == daAlink_c::PROC_ELEC_DAMAGE) {
            TwiliLog.info("[autotest] local player electrocuted");
            return true;
        }
    } else if (kind == "hurt") {
        link->setDamagePoint(step.value("value", 1), FALSE, TRUE, 0);
        TwiliLog.info("[autotest] local player hurt (damage timer {})", link->mDamageTimer);
        return true;
    } else if (kind == "chill") {
        link->mIceDamageWaitTimer = static_cast<s16>(step.value("value", 10));
        return ctx.ticks >= step.value("frames", 30);
    } else if (kind == "extinguish") {
        for (int i = 0; i < 4; i++) {
            link->clearFirePointDamageEffect(i);
        }
        return true;
    } else {
        ctx.fail("forceStatus: unknown kind '" + kind + "'");
        return false;
    }
    if (ctx.seconds > ctx.timeout(5.0)) {
        ctx.fail(fmt::format("forceStatus {}: did not take (proc 0x{:X})", kind,
                             static_cast<int>(link->mProcID)));
    }
    return false;
}

bool expectLocalStatus(StepContext& ctx) {
    const json& step = ctx.step;
    daAlink_c* link = daAlink_getAlinkActorClass();
    const RemoteStatusFx& s = statusfx::lastCaptured();
    std::string why;
    if (link == nullptr) {
        why = "no player";
    }
    int fires = 0;
    for (const RemoteFirePoint& f : s.fire) {
        fires += f.active() ? 1 : 0;
    }
    const int shield = dComIfGs_getSelectEquipShield();
    const bool inHand =
        link != nullptr && shield != dItemNo_NONE_e && !link->field_0x2e44.checkPassNum(15);
    auto want = [&](const char* key, auto have) {
        if (why.empty() && step.contains(key) && step[key].get<decltype(have)>() != have) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    want("frozen", (s.flags & kStatusFrozen) != 0);
    want("iceBlock", (s.flags & kStatusIceBlock) != 0);
    want("elec", (s.flags & kStatusElec) != 0);
    want("armorDrained", (s.flags & kStatusArmorDrained) != 0);
    want("firePoints", fires);
    want("shield", shield);
    want("shieldInHand", inHand);
    if (why.empty() && step.contains("shieldBurnMin") &&
        s.shieldBurn < step.value("shieldBurnMin", 0))
    {
        why = fmt::format("shield burn is {}", s.shieldBurn);
    }
    if (why.empty() && step.contains("shieldBurnMax") &&
        s.shieldBurn > step.value("shieldBurnMax", 0))
    {
        why = fmt::format("shield burn is {}", s.shieldBurn);
    }
    if (why.empty() && step.contains("damageTimerMin") &&
        s.damageTimer < step.value("damageTimerMin", 0))
    {
        why = fmt::format("damage timer is {}", s.damageTimer);
    }
    if (why.empty() && ((step.contains("sinkMin") && s.sinkOffset < step.value("sinkMin", 0.0f)) ||
                        (step.contains("sinkMax") && s.sinkOffset > step.value("sinkMax", 0.0f))))
    {
        why = fmt::format("sink is {:.1f}", s.sinkOffset);
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] local status: flags 0x{:X}, {} fire points, shield 0x{:X} burn {} "
                      "(burn-outs {}), damage timer {}, sink {:.1f} (proc 0x{:X})",
                      s.flags, fires, shield, s.shieldBurn, s.shieldBurnOutSeq, s.damageTimer,
                      s.sinkOffset, static_cast<int>(link->mProcID));
        return true;
    }
    if (ctx.seconds >= ctx.timeout(1.0)) {
        ctx.fail("expectLocalStatus: " + why);
    }
    return false;
}

// What of the step's expectations `info` misses, empty when none.
std::string statusMismatch(const json& step, const DummyPlayerDebugInfo& info) {
    std::string why;
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
    // A bool, or "ifAvailable": whatever this stage's pack can show.
    auto wantEffect = [&](const char* key, bool have, bool available) {
        if (!why.empty() || !step.contains(key)) {
            return;
        }
        const bool expected = step[key].is_string() ? available : step[key].get<bool>();
        if (have != expected) {
            why = fmt::format("{} is {} (pack has it: {})", key, have, available);
        }
    };
    want("frozen", info.frozen);
    wantEffect("iceBlock", info.iceBlock, iceBlockAvailable());
    want("thaws", info.thaws);
    atLeast("fireMin", info.firePoints);
    atMost("fireMax", info.firePoints);
    atLeast("fireEmittersMin", info.fireEmitters);
    atMost("fireEmittersMax", info.fireEmitters);
    want("fireReceived", static_cast<int>(info.fireReceived));
    atLeast("shieldBurnMin", info.shieldBurn);
    atMost("shieldBurnMax", info.shieldBurn);
    want("shieldBurnFx", info.shieldBurnFx);
    want("shieldBurnOuts", info.shieldBurnOuts);
    want("shieldItem", static_cast<int>(info.shieldItem));
    want("elec", info.elec);
    wantEffect("elecFx", info.elecFx, sparksAvailable());
    atLeast("damageTimerMin", info.damageTimer);
    atMost("damageTimerMax", info.damageTimer);
    atLeast("flashesMin", info.flashes);
    want("iceWait", static_cast<int>(info.iceWait));
    atLeast("sinkMin", info.sinkOffset);
    atMost("sinkMax", info.sinkOffset);
    want("statusFlags", static_cast<int>(info.statusFlags));
    return why;
}

// Tick at which the running expectDummyStatus first matched, -1 before.
int sStatusMatchedAt = -1;

bool expectDummyStatus(StepContext& ctx) {
    if (!ctx.begun) {
        sStatusMatchedAt = -1;
    }
    auto& session = Session::instance();
    DummyPlayerDebugInfo info;
    bool found = false;
    for (const auto& [id, c] : session.clients()) {
        fopAc_ac_c* dummy = peerInMyLayer(c) ? session.dummyActorForClient(id) : nullptr;
        if (dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info) && info.shellReady) {
            found = true;
            break;
        }
    }
    const std::string why = found ? statusMismatch(ctx.step, info) : "no peer dummy in our layer";
    if (why.empty()) {
        if (sStatusMatchedAt < 0) {
            sStatusMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sStatusMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] dummy status: flags 0x{:X}, frozen {} (ice {}, thaws {}), fire "
                      "{}/{} lit/emitting of {} received, shield 0x{:X} burn {} (fx {}, burn-outs "
                      "{}), elec {} (fx {}), damage {} ({} flashes), chill {}, sink {:.1f}",
                      info.statusFlags, info.frozen, info.iceBlock, info.thaws, info.firePoints,
                      info.fireEmitters, info.fireReceived, info.shieldItem, info.shieldBurn,
                      info.shieldBurnFx, info.shieldBurnOuts, info.elec, info.elecFx,
                      info.damageTimer, info.flashes, info.iceWait, info.sinkOffset);
        return true;
    }
    if (sStatusMatchedAt >= 0) {
        ctx.fail(fmt::format("expectDummyStatus: {} ticks after it matched, {}",
                             ctx.ticks - sStatusMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail("expectDummyStatus: " + why);
    }
    return false;
}

std::optional<bool> statusFxSteps(const std::string& op, StepContext& ctx) {
    if (op == "forceStatus") {
        return forceStatus(ctx);
    }

    if (op == "expectLocalStatus") {
        return expectLocalStatus(ctx);
    }

    if (op == "expectDummyStatus") {
        return expectDummyStatus(ctx);
    }

    if (op == "statusFxPack") {
        const bool ice = iceBlockAvailable();
        const bool sparks = sparksAvailable();
        const char* stage = dComIfGp_getStartStageName();
        TwiliLog.info("[autotest] status fx pack of {}: ice block {}, sparks {}",
                      stage != nullptr ? stage : "?", ice, sparks);
        if ((ctx.step.contains("iceBlock") && ctx.step["iceBlock"].get<bool>() != ice) ||
            (ctx.step.contains("elec") && ctx.step["elec"].get<bool>() != sparks))
        {
            ctx.fail(fmt::format("statusFxPack: ice block {}, sparks {}", ice, sparks));
            return false;
        }
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&statusFxSteps);

}  // namespace
}  // namespace twili::autotest
