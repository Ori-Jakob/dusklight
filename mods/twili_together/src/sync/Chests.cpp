// Chests and keys a teammate took: the lid opens here too (no event, no item), the key vanishes.

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "game/GameIdentity.hpp"

#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_tbox.h"
#include "d/actor/d_a_tbox2.h"
#include "d/d_bg_w.h"
#include "d/d_event.h"
#include "d/d_tresure.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <array>
#include <cstdint>
#include <iterator>

namespace twili::sync {
namespace detail {
namespace {

constexpr int kNoTbox = 0xFF;

// Chests whose lid we are opening, played from their own actionWait.
std::array<fpc_ProcID, 8> s_opening{};
size_t s_openingCount = 0;
// TBOX bits a teammate set while we were in that stage table: those lids open on screen.
uint64_t s_liveOpened = 0;

// A live one plays its lid and sound; one that came with a world state is simply open.
bool takeLive(int no) {
    const bool live = ((s_liveOpened >> no) & 1u) != 0;
    s_liveOpened &= ~(uint64_t{1} << no);
    return live;
}

bool liveTbox(int no) {
    if (no < 0 || no >= TBOX_MAX) {
        return false;
    }
    const dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    return ((static_cast<uint32_t>(bit.mTbox[no >> 5]) >> (no & 31)) & 1u) != 0;
}

// The player's open order for this chest is waiting: the event will command it next frame.
bool ordered(const fopAc_ac_c* actor) {
    const dEvt_control_c* ev = dComIfGp_getEvent();
    for (int i = 0; i < ev->mNum && i < static_cast<int>(std::size(ev->mOrder)); i++) {
        if (ev->mOrder[i].mpTargetActor == actor || ev->mOrder[i].mpRequestActor == actor) {
            return true;
        }
    }
    return false;
}

bool canOpen(fopAc_ac_c* actor, int tboxNo) {
    return tboxNo != kNoTbox && liveTbox(tboxNo) && enabled() && isSaveLoaded() &&
           !actor->eventInfo.checkCommandDoor() && !ordered(actor);
}

void track(fpc_ProcID id) {
    if (s_openingCount < s_opening.size()) {
        s_opening[s_openingCount++] = id;
    }
}

bool tracked(fpc_ProcID id) {
    for (size_t i = 0; i < s_openingCount; i++) {
        if (s_opening[i] == id) {
            return true;
        }
    }
    return false;
}

void untrack(fpc_ProcID id) {
    for (size_t i = 0; i < s_openingCount; i++) {
        if (s_opening[i] == id) {
            s_opening[i] = s_opening[--s_openingCount];
            return;
        }
    }
}

int tbox2No(daTbox2_c* chest) {
    // Only the randomizer saves these chests under a TBOX bit.
    if (game_identity::current().kind != "randomizer") {
        return kNoTbox;
    }
    return fopAcM_GetParamBit(chest, 16, 8);
}

void* removeTakenKey(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    if (fopAcM_GetName(actor) != fpcNm_Obj_SmallKey_e) {
        return nullptr;
    }
    auto* key = static_cast<daKey_c*>(actor);
    // Lying there or yet to appear; one we are picking up, or an enemy still holds, stays.
    if (key->getSaveBitNo() == *static_cast<int*>(data) &&
        (key->chkStatus(daKey_c::STATUS_WAIT_e) || key->chkStatus(daKey_c::STATUS_SW_ON_WAIT_e)))
    {
        TwiliLog.info("[sync] small key {} taken by a teammate: removed", key->getSaveBitNo());
        fopAcM_delete(key);
    }
    return nullptr;
}

}  // namespace

void onRemoteTbox(int saveTblNo, int tboxNo, bool live) {
    if (saveTblNo != currentSaveTblNo() || tboxNo < 0 || tboxNo >= TBOX_MAX) {
        return;
    }
    if (live) {
        s_liveOpened |= uint64_t{1} << tboxNo;
    }
    fopAcM_Search(&removeTakenKey, &tboxNo);
}

void clearOpeningChests() {
    s_openingCount = 0;
    s_liveOpened = 0;
}

}  // namespace detail

using namespace detail;

bool remoteOpenTbox(void* tbox) {
    auto* chest = static_cast<daTbox_c*>(tbox);
    const int no = chest->getTboxNo();
    if (!canOpen(chest, no)) {
        return false;
    }
    chest->eventInfo.offCondition(dEvtCnd_CANDOOR_e);
    chest->setAction(&daTbox_c::actionWait);
    chest->setDzb();
    if (chest->mpBgCollision != nullptr) {
        chest->mpBgCollision->Move();
    }
    dTres_c::offStatus(0, no, 1);
    const bool live = takeLive(no);
    if (live) {
        chest->mpAnm->setPlaySpeed(1.0f);
        fopAcM_seStart(chest, chest->mOpenSeId, 0);
        track(fopAcM_GetID(chest));
    } else {
        chest->mpAnm->setFrame(chest->mpAnm->getEndFrame());
    }
    TwiliLog.info("[sync] chest {} (shape {}) opened by a teammate{}", no, chest->getShapeType(),
        live ? "" : " earlier");
    return true;
}

void animateOpenedTbox(void* tbox) {
    auto* chest = static_cast<daTbox_c*>(tbox);
    if (s_openingCount == 0 || !tracked(fopAcM_GetID(chest))) {
        return;
    }
    chest->mpAnm->play();
    if (chest->mpAnm->getFrameCtrl()->checkState(1)) {
        untrack(fopAcM_GetID(chest));
    }
}

bool remoteOpenTbox2(void* tbox2) {
    auto* chest = static_cast<daTbox2_c*>(tbox2);
    const int no = tbox2No(chest);
    if (!canOpen(chest, no)) {
        return false;
    }
    chest->eventInfo.offCondition(dEvtCnd_CANDOOR_e);
    chest->init_actionWait();
    // As daTbox2_c::openInit.
    if (chest->mpBgW != nullptr) {
        dComIfG_Bgsp().Release(chest->mpBgW);
    }
    if (chest->mBoxBgW != nullptr) {
        dComIfG_Bgsp().Regist(chest->mBoxBgW, chest);
        chest->mBoxBgW->Move();
    }
    const bool live = takeLive(no);
    if (live) {
        chest->mpBck->setPlaySpeed(1.0f);
        fopAcM_seStart(chest,
            chest->mModelType == daTbox2_c::TYPE_SMALL_e ? Z2SE_OBJ_TBOX_OPEN_A : Z2SE_OBJ_TBOX_OPEN_B,
            0);
        track(fopAcM_GetID(chest));
    } else {
        chest->mpBck->setFrame(chest->mpBck->getEndFrame());
    }
    TwiliLog.info("[sync] chest {} opened by a teammate{}", no, live ? "" : " earlier");
    return true;
}

void animateOpenedTbox2(void* tbox2) {
    auto* chest = static_cast<daTbox2_c*>(tbox2);
    if (s_openingCount == 0 || !tracked(fopAcM_GetID(chest))) {
        return;
    }
    if (chest->mpBck->play()) {
        untrack(fopAcM_GetID(chest));
    }
}

}  // namespace twili::sync
