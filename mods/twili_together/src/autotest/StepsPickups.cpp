// Steps for chest sync, freestanding items, shared pickups and traps; reference in the runner README.

#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include "SSystem/SComponent/c_malloc.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_tbox.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_name.h"

#include <dolphin/pad.h>
#include <fmt/format.h>

#include <cmath>
#include <map>

namespace twili::autotest {
namespace {

std::map<std::string, fpc_ProcID> sChests;
std::map<std::string, fpc_ProcID> sFieldItems;

bool liveTbox(int no) {
    const dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    return no >= 0 && no < TBOX_MAX &&
           ((static_cast<uint32_t>(bit.mTbox[no >> 5]) >> (no & 31)) & 1u) != 0;
}

void* findChest(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    return fopAcM_GetName(actor) == fpcNm_TBOX_e &&
                   static_cast<daTbox_c*>(actor)->getTboxNo() == *static_cast<int*>(data) ?
               actor :
               nullptr;
}

daTbox_c* chestOf(const nlohmann::json& step) {
    if (step.contains("tag")) {
        const auto it = sChests.find(step.value("tag", std::string{}));
        if (it == sChests.end() || !fopAcM_IsExecuting(it->second)) {
            return nullptr;
        }
        return static_cast<daTbox_c*>(fopAcM_SearchByID(it->second));
    }
    int no = step.value("tbox", -1);
    return static_cast<daTbox_c*>(fopAcM_Search(&findChest, &no));
}

// A closed chest `dist` units in front of the player, facing him, so an A press would open it.
std::optional<bool> spawnChest(StepContext& ctx) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const int roomNo = dStage_roomControl_c::getStayNo();
    scene_class* room =
        roomNo >= 0 ? fopScnM_SearchByID(dStage_roomControl_c::getStatusProcID(roomNo)) : nullptr;
    if (player == nullptr || room == nullptr) {
        ctx.fail("spawnChest: no player or no room");
        return false;
    }
    fopAcM_prm_class* prm = fopAcM_CreateAppend();
    if (prm == nullptr) {
        ctx.fail("spawnChest: out of memory");
        return false;
    }
    const int tbox = ctx.step.value("tbox", 60) & 0x3F;
    const int shape = ctx.step.value("shape", 0) & 0xF;
    const int item = ctx.step.value("item", 0x04) & 0xFF;
    const float dist = ctx.step.value("dist", 110.0f);
    prm->base.parameters = (0xFFu << 24) | (static_cast<u32>(shape) << 20) | (0xFFu << 12) |
                           (static_cast<u32>(tbox) << 6);
    // Most level ground nearest ahead: sunk in a slope a chest hides, raised it is out of reach.
    const cXyz at = player->current.pos;
    s16 facing = player->shape_angle.y;
    cXyz pos(at.x + dist * cM_ssin(facing), at.y, at.z + dist * cM_scos(facing));
    f32 bestDy = 1e9f;
    for (int i = 0; i < 16 && bestDy > 5.0f; i++) {
        const int turn = (i + 1) / 2 * (i % 2 != 0 ? 0x1000 : -0x1000);
        const s16 yaw = static_cast<s16>(player->shape_angle.y + turn);
        dBgS_ObjGndChk gnd;
        const cXyz probe(at.x + dist * cM_ssin(yaw), at.y + 100.0f, at.z + dist * cM_scos(yaw));
        gnd.SetPos(&probe);
        const f32 groundY = dComIfG_Bgsp().GroundCross(&gnd);
        if (groundY != -G_CM3D_F_INF && std::fabs(groundY - at.y) < bestDy) {
            bestDy = std::fabs(groundY - at.y);
            pos.set(probe.x, groundY, probe.z);
            facing = yaw;
        }
    }
    if (facing != player->shape_angle.y) {
        daPy_getPlayerActorClass()->setPlayerPosAndAngle(&at, facing, TRUE);
    }
    prm->base.position = pos;
    prm->base.angle = csXyz(0, static_cast<s16>(facing + 0x8000), static_cast<s16>(item << 8 | 0xFF));
    prm->base.setID = 0xFFFF;
    prm->room_no = static_cast<s8>(roomNo);
    prm->argument = -1;
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(&room->base.layer);
    const fpc_ProcID id = fopAcM_Create(fpcNm_TBOX_e, NULL, prm);
    fpcLy_SetCurrentLayer(saved);
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        cMl::free(prm);
        ctx.fail("spawnChest: create request failed");
        return false;
    }
    sChests[ctx.step.value("tag", std::string("chest"))] = id;
    TwiliLog.info("[autotest] chest {} (shape {}, item 0x{:02X}) requested, ground {:+.0f}", tbox,
        shape, item, pos.y - at.y);
    return true;
}

// What Link does on an A press next to it, pressing A through the item message; done once the
// bit is set and the event ended. refused: for `frames` ticks the open must not start an event.
std::optional<bool> openChest(StepContext& ctx) {
    daTbox_c* chest = chestOf(ctx.step);
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const bool refused = ctx.step.value("refused", false);
    const bool running = dComIfGp_event_runCheck() != FALSE;
    if (refused && running) {
        ctx.fail("openChest: the open started an event");
        return false;
    }
    if (refused && ctx.ticks >= ctx.step.value("frames", 60)) {
        return true;
    }
    if (!refused && chest != nullptr && liveTbox(chest->getTboxNo()) && !running) {
        return true;
    }
    if (chest != nullptr && player != nullptr && !running) {
        fopAcM_orderTreasureEvent(player, chest, 0, 0);
    } else if (running && !padBusy() && ctx.ticks % 20 == 0) {
        pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
    }
    if (ctx.seconds > ctx.timeout(45.0)) {
        ctx.fail(chest == nullptr ? std::string("openChest: no such chest") :
                                    fmt::format("openChest: chest {} did not open", chest->getTboxNo()));
    }
    return false;
}

// open: TBOX bit set and the lid at its last frame, with no event of ours.
std::optional<bool> expectChestOpen(StepContext& ctx) {
    daTbox_c* chest = chestOf(ctx.step);
    const bool want = ctx.step.value("open", true);
    if (chest != nullptr) {
        const bool bit = liveTbox(chest->getTboxNo());
        const bool lid = chest->mpAnm->getFrame() >= chest->mpAnm->getEndFrame() - 0.5f;
        const bool open = bit && lid;
        if (open == want && (!want || !dComIfGp_event_runCheck())) {
            TwiliLog.info("[autotest] chest {} open={} (bit {}, frame {:.1f})", chest->getTboxNo(),
                open, bit, chest->mpAnm->getFrame());
            return !want || ctx.seconds >= ctx.step.value("forSec", 0.0);
        }
        if (!want && open) {
            ctx.fail(fmt::format("chest {} opened", chest->getTboxNo()));
            return false;
        }
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(chest == nullptr ? std::string("expectChestOpen: no such chest") :
                                    fmt::format("chest {}: bit {} frame {:.1f}/{:.1f}",
                                        chest->getTboxNo(), liveTbox(chest->getTboxNo()),
                                        chest->mpAnm->getFrame(), chest->mpAnm->getEndFrame()));
    }
    return false;
}

fopAc_ac_c* fieldItemOf(const nlohmann::json& step) {
    const auto it = sFieldItems.find(step.value("tag", std::string("item")));
    if (it == sFieldItems.end() || !fopAcM_IsExecuting(it->second)) {
        return nullptr;
    }
    return fopAcM_SearchByID(it->second);
}

// A field item, or a heart piece for items 0x21/0x22, with item bit `bit`, `dist` units ahead.
std::optional<bool> spawnFieldItem(StepContext& ctx) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const int roomNo = dStage_roomControl_c::getStayNo();
    scene_class* room =
        roomNo >= 0 ? fopScnM_SearchByID(dStage_roomControl_c::getStatusProcID(roomNo)) : nullptr;
    if (player == nullptr || room == nullptr) {
        ctx.fail("spawnFieldItem: no player or no room");
        return false;
    }
    const int item = ctx.step.value("item", 0x01) & 0xFF;
    const int bit = ctx.step.value("bit", 0x9C) & 0xFF;
    const float dist = ctx.step.value("dist", 150.0f);
    const s16 facing = player->shape_angle.y;
    const cXyz pos(player->current.pos.x + dist * cM_ssin(facing), player->current.pos.y + 20.0f,
        player->current.pos.z + dist * cM_scos(facing));
    const csXyz angle(0, facing, 0);
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(&room->base.layer);
    const fpc_ProcID id =
        fopAcM_createItem(&pos, item, bit, roomNo, &angle, nullptr, daItem_c::TYPE_WAIT_e);
    fpcLy_SetCurrentLayer(saved);
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        ctx.fail("spawnFieldItem: create request failed");
        return false;
    }
    sFieldItems[ctx.step.value("tag", std::string("item"))] = id;
    TwiliLog.info("[autotest] field item 0x{:02X} (bit {}) requested", item, bit);
    return true;
}

// Puts the item on Link until he takes it, pressing A through the item message after `holdSec`;
// done once the item is gone and no event runs.
std::optional<bool> takeFieldItem(StepContext& ctx) {
    fopAc_ac_c* item = fieldItemOf(ctx.step);
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const bool running = dComIfGp_event_runCheck() != FALSE;
    if (item == nullptr && !running) {
        return true;
    }
    if (item != nullptr && player != nullptr && !running) {
        item->current.pos = player->current.pos;
    } else if (running && !padBusy() && ctx.seconds >= ctx.step.value("holdSec", 0.0) &&
               ctx.ticks % 20 == 0)
    {
        pulsePad(0.0f, 0.0f, PAD_BUTTON_A, 3);
    }
    if (ctx.seconds > ctx.timeout(45.0)) {
        ctx.fail("takeFieldItem: the item was never taken");
    }
    return false;
}

// Marks the item as pulled by the clawshot (hold: true) or lets it go, as a busy item.
std::optional<bool> holdFieldItem(StepContext& ctx) {
    fopAc_ac_c* item = fieldItemOf(ctx.step);
    if (item == nullptr) {
        ctx.fail("holdFieldItem: no such item");
        return false;
    }
    if (ctx.step.value("hold", true)) {
        fopAcM_setHookCarryNow(item);
    } else {
        fopAcM_cancelHookCarryNow(item);
    }
    return true;
}

std::optional<bool> expectFieldItem(StepContext& ctx) {
    const bool want = ctx.step.value("present", true);
    if ((fieldItemOf(ctx.step) != nullptr) == want) {
        return ctx.seconds >= ctx.step.value("forSec", 0.0);
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("field item '{}' present={}, want {}",
            ctx.step.value("tag", std::string("item")), !want, want));
    }
    return false;
}

// A dungeon (below 0x80) or memory item bit; `tbl` reads a memory bit from that stage's save slot.
std::optional<bool> expectItemBit(StepContext& ctx) {
    const int bit = ctx.step.value("bit", -1);
    const int tbl = ctx.step.value("tbl", -1);
    const bool want = ctx.step.value("set", true);
    dSv_info_c* info = dComIfGs_getSaveInfo();
    bool have = false;
    if (bit >= 0 && bit < dSv_info_c::MEMORY_ITEM) {
        have = info->getDan().isItem(bit) != FALSE;
    } else if (bit >= dSv_info_c::MEMORY_ITEM && bit < dSv_info_c::MEMORY_ITEM + dSv_info_c::DAN_ITEM) {
        const bool live = tbl < 0 || tbl == Session::instance().currentSaveTblNo();
        const dSv_memBit_c& mem = live || tbl >= dSv_save_c::STAGE_MAX ?
                                      info->getMemory().getBit() :
                                      info->getSavedata().getSave(tbl).getBit();
        have = mem.isItem(bit - dSv_info_c::MEMORY_ITEM) != FALSE;
    }
    if (have == want) {
        return ctx.seconds >= ctx.step.value("forSec", 0.0);
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("item bit {} (table {}) never became {}", bit, tbl, want));
    }
    return false;
}

// The bit in stage table `tbl` (default: the live one of the current stage).
std::optional<bool> expectTbox(StepContext& ctx) {
    const int no = ctx.step.value("no", -1);
    const int tbl = ctx.step.value("tbl", -1);
    const bool want = ctx.step.value("set", true);
    bool have = liveTbox(no);
    if (tbl >= 0 && tbl < dSv_save_c::STAGE_MAX && tbl != Session::instance().currentSaveTblNo()) {
        have = no >= 0 && no < TBOX_MAX &&
               dComIfGs_getSaveInfo()->getSavedata().getSave(tbl).getBit().isTbox(no) != FALSE;
    }
    if (have == want) {
        return ctx.seconds >= ctx.step.value("forSec", 0.0);
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("TBOX bit {} (table {}) never became {}", no, tbl, want));
    }
    return false;
}

// Stats fields: equal to `name`, or at least `min.name`, and at most `max.name`.
std::optional<bool> expectPickups(StepContext& ctx) {
    const sync::Stats& s = sync::stats();
    const std::map<std::string, int64_t> have = {
        {"sent", s.pickupsSent}, {"applied", s.pickupsApplied}, {"skipped", s.pickupsSkipped},
        {"rupees", s.rupeesAdded}, {"life", s.lifeAdded}, {"arrows", s.arrowsAdded},
        {"seeds", s.seedsAdded}, {"bombs", s.bombsAdded}, {"trapsSent", s.trapsSent},
        {"trapsReceived", s.trapsReceived}, {"trapsApplied", s.trapsApplied},
        {"grantsApplied", s.grantsApplied}, {"grantsQueued", s.grantsQueued},
        {"removed", s.freestandingRemoved}, {"blocked", s.freestandingBlocked},
        {"deduped", s.grantsDeduped},
    };
    std::string bad;
    for (const auto& [name, value] : have) {
        if (ctx.step.contains(name) && value != ctx.step[name].get<int64_t>()) {
            bad += fmt::format(" {}={} (want {})", name, value, ctx.step[name].get<int64_t>());
        }
        if (ctx.step.contains("min") && ctx.step["min"].contains(name) &&
            value < ctx.step["min"][name].get<int64_t>())
        {
            bad += fmt::format(" {}={} (want >= {})", name, value, ctx.step["min"][name].get<int64_t>());
        }
        if (ctx.step.contains("max") && ctx.step["max"].contains(name) &&
            value > ctx.step["max"][name].get<int64_t>())
        {
            ctx.fail(fmt::format("expectPickups: {}={} above {}", name, value,
                ctx.step["max"][name].get<int64_t>()));
            return false;
        }
    }
    if (bad.empty()) {
        return ctx.seconds >= ctx.step.value("forSec", 0.0);
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectPickups:" + bad);
    }
    return false;
}

std::optional<bool> setInventory(StepContext& ctx) {
    const nlohmann::json& step = ctx.step;
    if (step.contains("rupees")) {
        dComIfGs_setRupee(static_cast<u16>(step.value("rupees", 0)));
    }
    if (step.contains("life")) {
        dComIfGs_setLife(static_cast<u16>(step.value("life", 12)));
    }
    if (step.contains("arrows")) {
        dComIfGs_setArrowNum(static_cast<u8>(step.value("arrows", 0)));
    }
    if (step.contains("maxLife")) {
        dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusA().setMaxLife(
            static_cast<u16>(step.value("maxLife", 15)));
    }
    return true;
}

// rupees, life (quarter hearts), arrows, bombs (bag slot 0), seeds, poes; `keys` of stage table `tbl`.
std::optional<bool> expectInventory(StepContext& ctx) {
    const nlohmann::json& step = ctx.step;
    dSv_info_c* info = dComIfGs_getSaveInfo();
    const int tbl = step.value("tbl", -1);
    const int keys = tbl < 0 ? -1 :
                     tbl == Session::instance().currentSaveTblNo() ? info->getMemory().getBit().mKeyNum :
                                                   info->getSavedata().getSave(tbl).getBit().mKeyNum;
    int bottles = 0;
    for (int i = 0; i < dSv_player_item_c::BOTTLE_MAX; i++) {
        bottles += dComIfGs_getItem(SLOT_11 + i, false) != dItemNo_NONE_e;
    }
    const std::map<std::string, int> have = {
        {"rupees", dComIfGs_getRupee()}, {"life", dComIfGs_getLife()},
        {"arrows", dComIfGs_getArrowNum()}, {"bombs", dComIfGs_getBombNum(0)},
        {"seeds", dComIfGs_getPachinkoNum()}, {"poes", dComIfGs_getPohSpiritNum()},
        {"keys", keys}, {"bottles", bottles},
        {"maxLife", info->getPlayer().getPlayerStatusA().getMaxLife()},
    };
    std::string bad;
    for (const auto& [name, value] : have) {
        if (step.contains(name) && value != step[name].get<int>()) {
            bad += fmt::format(" {}={} (want {})", name, value, step[name].get<int>());
        }
    }
    if (bad.empty()) {
        return ctx.seconds >= step.value("forSec", 0.0);
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectInventory:" + bad);
    }
    return false;
}

std::optional<bool> pickupSteps(const std::string& op, StepContext& ctx) {
    if (op == "spawnChest") {
        return spawnChest(ctx);
    }
    if (op == "openChest") {
        return openChest(ctx);
    }
    if (op == "expectChestOpen") {
        return expectChestOpen(ctx);
    }
    if (op == "expectTbox") {
        return expectTbox(ctx);
    }
    if (op == "spawnFieldItem") {
        return spawnFieldItem(ctx);
    }
    if (op == "takeFieldItem") {
        return takeFieldItem(ctx);
    }
    if (op == "holdFieldItem") {
        return holdFieldItem(ctx);
    }
    if (op == "expectFieldItem") {
        return expectFieldItem(ctx);
    }
    if (op == "expectItemBit") {
        return expectItemBit(ctx);
    }
    if (op == "expectPickups") {
        return expectPickups(ctx);
    }
    if (op == "setInventory") {
        return setInventory(ctx);
    }
    if (op == "expectInventory") {
        return expectInventory(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&pickupSteps);

}  // namespace
}  // namespace twili::autotest
