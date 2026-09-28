// Steps for how remote players look (actors/DummyPlayer.cpp, presence/PlayerUpdate.cpp).
//
// setClothes  item (0x2E Ordon; 0x2F Kokiri, 0x30 Magic Armor, 0x31 Zora), timeoutSec (20)
//     As the collection screen does; done once the new body is bound.
// setBoots  on (true)
//     Iron boots on (assigned to X) or off; done once that has held for 10 ticks.
// patchPlayerUpdate  patch ({}), packets (60)
//     Merges `patch` into our next `packets` PLAYER_UPDATEs, each one a keyframe.
// assignItemX  item
//     An owned item (dItemNo_*) on the X button (a walk step's buttons: X = 0x400).
// expectDummyLook  clothes, casualHead, heavyBoots, zoraMask, lantern, heldItem, basePack,
//                  basePackAnm, standIn, ground, hidden, armorDrained, armorSettled, maxMissing;
//                  frames (0), timeoutSec (20)
//     The first peer dummy in our layer shows every field given, then for `frames` more ticks.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_msg_mng.h"

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

bool setClothes(StepContext& ctx) {
    const int item = ctx.step.value("item", static_cast<int>(dItemNo_WEAR_CASUAL_e));
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (!ctx.begun) {
        if (link == nullptr || link->checkWolf()) {
            ctx.fail("setClothes: no human player");
            return false;
        }
        dMeter2Info_setCloth(static_cast<u8>(item), false);
        link->setClothesChange(0);
    }
    // loadModelDVD needs a few ticks before it starts the swap.
    if (link != nullptr && ctx.ticks > 4 && link->getClothesChangeWaitTimer() == 0 &&
        dComIfGs_getSelectEquipClothes() == item)
    {
        TwiliLog.info("[autotest] local player wears 0x{:X}", item);
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("setClothes: still no 0x{:X} body", item));
    }
    return false;
}

bool setBoots(StepContext& ctx) {
    const bool on = ctx.step.value("on", true);
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || link->checkWolf()) {
        ctx.fail("setBoots: no human player");
        return false;
    }
    if (!ctx.begun) {
        if (on) {
            // execute takes the boots off again unless they are on X or Y.
            int slot = -1;
            for (int i = 0; i < 24 && slot < 0; i++) {
                if (dComIfGs_getItem(i, false) == dItemNo_HVY_BOOTS_e) slot = i;
            }
            for (int i = 0; i < 24 && slot < 0; i++) {
                if (fopMsgM_itemNumIdx(i) == dItemNo_HVY_BOOTS_e) {
                    dComIfGs_setItem(i, dItemNo_HVY_BOOTS_e);
                    slot = i;
                }
            }
            if (slot < 0) {
                ctx.fail("setBoots: no item slot for the iron boots");
                return false;
            }
            dComIfGs_setSelectItemIndex(0, static_cast<u8>(slot));
        }
        // setHeavyBoots toggles whatever its argument.
        if ((link->checkEquipHeavyBoots() != 0) != on) {
            link->setHeavyBoots(on ? 1 : 0);
        }
    }
    if (ctx.begun && (link->checkEquipHeavyBoots() != 0) != on) {
        ctx.fail(fmt::format("setBoots: the boots came back {}", on ? "off" : "on"));
        return false;
    }
    return ctx.ticks >= 10;
}

// What of the step's expectations `info` misses, empty when none.
std::string lookMismatch(const json& step, const DummyPlayerDebugInfo& info) {
    std::string why;
    auto want = [&](const char* key, auto have) {
        if (why.empty() && step.contains(key) && step[key].get<decltype(have)>() != have) {
            why = fmt::format("{} is {}", key, have);
        }
    };
    want("clothes", static_cast<int>(info.clothes));
    want("casualHead", info.casualHead);
    want("heavyBoots", info.heavyBoots);
    want("zoraMask", info.zoraMask);
    want("lantern", info.lantern);
    want("heldItem", static_cast<int>(info.heldItem));
    want("basePack", info.basePack);
    want("basePackAnm", static_cast<int>(info.basePackAnm));
    want("standIn", info.standIn);
    want("ground", info.ground);
    want("hidden", info.hidden);
    want("armorDrained", info.armorDrained);
    if (why.empty() && step.contains("armorSettled") &&
        step["armorSettled"].get<bool>() != info.armorBrkSettled)
    {
        why = fmt::format("armor BRK at frame {:.0f}", info.armorBrkFrame);
    }
    if (why.empty() && step.contains("maxMissing") &&
        info.missingAnms > step["maxMissing"].get<uint32_t>())
    {
        why = fmt::format("{} clips were missing", info.missingAnms);
    }
    return why;
}

// Tick at which the running expectDummyLook first matched, -1 before.
int sLookMatchedAt = -1;

bool expectDummyLook(StepContext& ctx) {
    if (!ctx.begun) {
        sLookMatchedAt = -1;
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
    const std::string why = found ? lookMismatch(ctx.step, info) : "no peer dummy in our layer";
    if (why.empty()) {
        if (sLookMatchedAt < 0) {
            sLookMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sLookMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] dummy look: clothes 0x{:X}, casual head {}, boots {}, zora mask "
                      "{}, lantern {}, held 0x{:X}, base clip 0x{:X}{}, ground {}, {} missing, "
                      "armor drained {} (BRK frame {:.0f})",
                      info.clothes, info.casualHead, info.heavyBoots, info.zoraMask,
                      info.lantern, info.heldItem, info.basePackAnm,
                      info.standIn ? " (stand-in)" : "", info.ground, info.missingAnms,
                      info.armorDrained, info.armorBrkFrame);
        return true;
    }
    if (sLookMatchedAt >= 0) {
        ctx.fail(fmt::format("expectDummyLook: {} ticks after it matched, {}",
                             ctx.ticks - sLookMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectDummyLook: " + why);
    }
    return false;
}

std::optional<bool> fidelitySteps(const std::string& op, StepContext& ctx) {
    if (op == "setClothes") {
        return setClothes(ctx);
    }

    if (op == "setBoots") {
        return setBoots(ctx);
    }

    if (op == "assignItemX") {
        const int item = ctx.step.value("item", 0);
        for (int i = 0; i < 24; i++) {
            if (dComIfGs_getItem(i, false) == item) {
                dComIfGs_setSelectItemIndex(0, static_cast<u8>(i));
                return true;
            }
        }
        ctx.fail(fmt::format("assignItemX: item 0x{:X} is not owned", item));
        return false;
    }

    if (op == "patchPlayerUpdate") {
        Session::setPlayerUpdateTestPatch(ctx.step.value("patch", json::object()),
                                          ctx.step.value("packets", 60));
        return true;
    }

    if (op == "expectDummyLook") {
        return expectDummyLook(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&fidelitySteps);

}  // namespace
}  // namespace twili::autotest
