#include "core/LocalPlayer.hpp"

#include "core/Maps.hpp"
#include "core/SaveGate.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_overlap_mng.h"

#include <cstring>

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

bool isTeleportableRoom(const char* stage, int roomNo) {
    // The safe-spot capture asks about the same room every tick.
    static char s_stage[8] = {};
    static int s_room = -1;
    static bool s_result = false;
    if (s_room == roomNo && std::strncmp(s_stage, stage, sizeof(s_stage)) == 0) {
        return s_result;
    }
    std::strncpy(s_stage, stage, sizeof(s_stage) - 1);
    s_room = roomNo;
    s_result = false;
    for (size_t i = 0; i < maps::kRoomCount && !s_result; i++) {
        const maps::Room& r = maps::kRooms[i];
        s_result = r.room >= 0 && r.room == roomNo && std::strcmp(r.stage, stage) == 0;
    }
    return s_result;
}

bool isKnownEntrance(const char* stage, int roomNo, int point) {
    for (size_t i = 0; i < maps::kRoomCount; i++) {
        const maps::Room& r = maps::kRooms[i];
        if (r.room < 0 || r.room != roomNo || std::strcmp(r.stage, stage) != 0) {
            continue;
        }
        for (uint8_t p = 0; p < r.pointCount; p++) {
            if (r.points[p] == point) {
                return true;
            }
        }
    }
    return false;
}

std::string mapName(const char* stage, int roomNo) {
    const char* anyRoom = nullptr;
    for (size_t i = 0; i < maps::kRoomCount; i++) {
        const maps::Room& r = maps::kRooms[i];
        if (std::strcmp(r.stage, stage) != 0) {
            continue;
        }
        if (anyRoom == nullptr) {
            anyRoom = r.name;
        }
        if (r.room >= 0 && r.room == roomNo) {
            return r.name;
        }
    }
    return anyRoom != nullptr ? anyRoom : stage;
}

namespace {
const OwnStageRequest* s_ownRequest = nullptr;
}  // namespace

OwnStageRequest::OwnStageRequest(const char* s, int p, int r, int l)
    : point(static_cast<int16_t>(p)), room(static_cast<int8_t>(r)), layer(static_cast<int8_t>(l)) {
    std::strncpy(stage, s, sizeof(stage) - 1);
    s_ownRequest = this;
}

OwnStageRequest::~OwnStageRequest() {
    if (s_ownRequest == this) {
        s_ownRequest = nullptr;
    }
}

const OwnStageRequest* OwnStageRequest::current() {
    return s_ownRequest;
}

}  // namespace twili::local
