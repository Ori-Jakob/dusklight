#include "core/LocalPlayer.hpp"

#include "core/SaveGate.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_overlap_mng.h"

namespace twili::local {

daAlink_c* liveLink() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    return link != nullptr && fopAcM_IsExecuting(fopAcM_GetID(link)) ? link : nullptr;
}

bool isSettledOnGround(daAlink_c* link) {
    return link->mLinkAcch.ChkGroundHit() && !link->checkPlayerFly() &&
           !link->checkMagneBootsOn();
}

bool onSafeFloor(daAlink_c* link) {
    return isSettledOnGround(link) && link->mGroundCode != 4 && link->mGroundCode != 9 &&
           link->mGroundCode != 10 && link->mGndPolyAtt1 != 2;
}

const char* localTeleportBlocker() {
    if (!isSaveLoaded()) {
        return "not-in-game";
    }
    daAlink_c* link = liveLink();
    if (link == nullptr || dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() ||
        fopOvlpM_IsDoingReq())
    {
        return "loading";
    }
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0) {
        return "menu";
    }
    if (dComIfGp_event_runCheck() || link->mProcID == daAlink_c::PROC_WARP) {
        return "cutscene";
    }
    if (link->mProcID == daAlink_c::PROC_DEAD || dComIfGs_getLife() == 0) {
        return "down";
    }
    if (link->checkRide() || link->checkModeFlg(daAlink_c::MODE_RIDING)) {
        return "riding";
    }
    // Sumo, goat herding, cargo escort, wolf puzzle.
    if (link->mMode != 0) {
        return "busy";
    }
    // A carried light ball makes the next create() wait for a ball that is not there.
    if (link->mGrabItemAcKeep.getActor() != nullptr) {
        return "carrying";
    }
    return nullptr;
}

}  // namespace twili::local
