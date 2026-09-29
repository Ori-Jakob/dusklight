// LIGHT_DROP: each tear picked adds one to every teammate's count, once (keyed by its TBOX bit).

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "d/actor/d_a_obj_drop.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

namespace twili::sync {
namespace detail {
namespace {

constexpr uint8_t kMaxTears = 16;
constexpr int kAreas = 4;

struct Pickup {
    const void* drop = nullptr;
    int area = -1;
    int tbox = -1;
    int saveTbl = -1;
    uint8_t countBefore = 0;
    bool bitBefore = false;
};

// Set while a dropGet runs.
Pickup s_pickup;
bool s_keepTakenTears = false;

bool rawTbox(const dSv_memBit_c& bit, int no) {
    return ((static_cast<uint32_t>(bit.mTbox[no >> 5]) >> (no & 31)) & 1u) != 0;
}

dSv_memBit_c* tboxBits(int saveTblNo) {
    if (saveTblNo < 0 || saveTblNo >= dSv_save_c::STAGE_MAX) {
        return nullptr;
    }
    dSv_info_c* info = dComIfGs_getSaveInfo();
    if (saveTblNo == currentSaveTblNo()) {
        return &info->getMemory().getBit();
    }
    return &info->getSavedata().getSave(saveTblNo).getBit();
}

void* findDrop(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (fopAcM_GetName(actor) != fpcNm_Obj_Drop_e) {
        return nullptr;
    }
    return static_cast<daObjDrop_c*>(actor)->getSave() == *static_cast<int*>(data) ? actor : nullptr;
}

}  // namespace

void handleLightDrop(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "LIGHT_DROP")) {
        return;
    }
    const int area = packet.value("area", -1);
    const int saveTbl = packet.value("tbl", -1);
    int tbox = packet.value("tbox", -1);
    dSv_memBit_c* bit = tboxBits(saveTbl);
    if (area < 0 || area >= kAreas || tbox < 0 || tbox >= TBOX_MAX || bit == nullptr) {
        return;
    }
    if (rawTbox(*bit, tbox)) {
        return;  // this tear already counted here
    }
    RemoteApplyGuard guard;
    bit->onTbox(tbox);
    const uint8_t num = dComIfGs_getLightDropNum(static_cast<u8>(area));
    if (num < kMaxTears) {
        dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(num + 1));
    }
    // The tear this teammate took vanishes here too.
    if (saveTbl == currentSaveTblNo() && !s_keepTakenTears) {
        if (auto* drop = static_cast<fopAc_ac_c*>(fopAcM_Search(&findDrop, &tbox))) {
            fopAcM_delete(drop);
        }
    }
    TwiliLog.info("[sync] tear {} of area {} taken by {}: count {} -> {}", tbox, area,
        packet.value("senderName", std::string{}), num, dComIfGs_getLightDropNum(static_cast<u8>(area)));
}

}  // namespace detail

using namespace detail;

void beforeDropGet(void* drop) {
    auto* d = static_cast<daObjDrop_c*>(drop);
    s_pickup = {};
    if (RemoteApplyGuard::active() || !isSaveLoaded() || !d->mSetCollectDrop) {
        return;
    }
    const int area = dComIfGp_getStartStageDarkArea();
    dSv_memBit_c* bit = tboxBits(currentSaveTblNo());
    if (area < 0 || area >= kAreas || bit == nullptr) {
        return;
    }
    s_pickup.drop = drop;
    s_pickup.area = area;
    s_pickup.tbox = d->getSave();
    s_pickup.saveTbl = currentSaveTblNo();
    s_pickup.countBefore = dComIfGs_getLightDropNum(static_cast<u8>(area));
    s_pickup.bitBefore = rawTbox(*bit, s_pickup.tbox);
}

void afterDropGet(void* drop) {
    const Pickup p = s_pickup;
    s_pickup = {};
    if (p.drop == nullptr || p.drop != drop) {
        return;
    }
    const uint8_t num = dComIfGs_getLightDropNum(static_cast<u8>(p.area));
    if (num <= p.countBefore) {
        return;
    }
    if (p.bitBefore) {
        // A teammate took this tear a moment ago and it already counted: dropGet adds it again.
        dComIfGs_setLightDropNum(static_cast<u8>(p.area), p.countBefore);
        TwiliLog.info("[sync] tear {} of area {} was already counted; count stays {}", p.tbox,
            p.area, p.countBefore);
        return;
    }
    TwiliLog.info("[sync] tear {} of area {} picked: count {}", p.tbox, p.area, num);
    if (!enabled()) {
        return;
    }
    nlohmann::json packet = {
        {"type", "LIGHT_DROP"},
        {"area", p.area},
        {"tbl", p.saveTbl},
        {"tbox", p.tbox},
    };
    stampWorldPacket(packet, true);
    send(std::move(packet));
}

bool insideDropGet() {
    return s_pickup.drop != nullptr;
}

#if TWILI_ENABLE_AUTOTEST
void keepTakenTearsForTest(bool keep) {
    s_keepTakenTears = keep;
}
#endif

}  // namespace twili::sync
