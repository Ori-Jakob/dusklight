#include "fx/WolfFx.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/d_com_inf_game.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/Wmdl.h"

#include <algorithm>
#include <cmath>

namespace twili::wolffx {
namespace {

// Entries into PROC_WOLF_LOCK_ATTACK, one LOCKATDASH burst each.
struct Tracker {
    int prevProc = -1;
    uint8_t lockDashSeq = 0;
};

Tracker s_tx;
RemoteWolfFx s_last;

}  // namespace

void track() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    const int proc = link != nullptr && link->checkWolf() ? static_cast<int>(link->mProcID) : -1;
    if (proc == daAlink_c::PROC_WOLF_LOCK_ATTACK && s_tx.prevProc != proc) {
        s_tx.lockDashSeq++;
    }
    s_tx.prevProc = proc;
}

bool captureLocal(const RemoteMidnaPose& midna, RemoteWolfFx& out) {
    out = RemoteWolfFx{};
    out.lockDashSeq = s_tx.lockDashSeq;
    daAlink_c* link = daAlink_getAlinkActorClass();
    // Nothing crosses a body swap (loadModelDVD frees the old model data meanwhile).
    if (link == nullptr || !link->checkWolf() || link->getClothesChangeWaitTimer() != 0) {
        s_last = out;
        return false;
    }
    switch (link->mProcID) {
    case daAlink_c::PROC_WOLF_ROLL_ATTACK:  // setEffect's condition for setWolfRollAttackEffect
        out.flags |= kWolfFxSpin;
        if (link->mProcVar2.field_0x300c != 0) {
            out.flags |= kWolfFxSpinRight;
        }
        if (link->mProcVar3.field_0x300e != 0) {
            out.flags |= kWolfFxSpinNoTrail;
        }
        break;
    case daAlink_c::PROC_WOLF_ROLL_ATTACK_CHARGE:
    case daAlink_c::PROC_WOLF_ROLL_ATTACK_MOVE:
        out.flags |= kWolfFxCharge;
        break;
    default:
        break;
    }
    // setWolfLockDomeModel's model
    if (link->mEquipItem == kLockDomeItem && link->mHeldItemModel != nullptr &&
        std::isfinite(link->mSearchBallScale))
    {
        out.flags |= kWolfFxDome;
        out.domeRadius = link->mSearchBallScale;
    }
    // setWolfLockAttackEffect's condition.
    if (dComIfGp_checkPlayerStatus1(0, 0x01000000)) {
        out.flags |= kWolfFxLockBlur;
    }
    out.lockCount = link->mWolfLockNum;
    // daMidna_c::setBodyPartMatrix
    fopAc_ac_c* target = link->getWolfLockActorEnd();
    J3DModel* md = link->getMidnaModel();
    daMidna_c* m = daPy_py_c::getMidnaActor();
    if (midna.mode != kMidnaNone && (midna.flags & kMidnaHairFromBck) && target != nullptr &&
        md != nullptr && m != nullptr)
    {
        cXyz base;
        mDoMtx_stack_c::copy(md->getAnmMtx(MD_JNT_HAIR_5_e));
        mDoMtx_stack_c::transM(6.5f, 0.0f, 0.0f);
        mDoMtx_stack_c::multVecZero(&base);
        const cXyz toTarget = target->eyePos - base;
        out.hairAim = static_cast<int16_t>(toTarget.atan2sX_Z() - m->shape_angle.y);
        out.flags |= kWolfFxHairAim;
    }
    s_last = out;
    return out.flags != 0;
}

const RemoteWolfFx& lastCaptured() {
    return s_last;
}

void encode(const RemoteWolfFx& f, int32_t wx[4]) {
    wx[0] = f.flags;
    wx[1] = (f.flags & kWolfFxDome) ? static_cast<int32_t>(std::lround(f.domeRadius)) : 0;
    wx[2] = f.lockDashSeq | (f.lockCount << 8);
    wx[3] = (f.flags & kWolfFxHairAim) ? f.hairAim : 0;
}

RemoteWolfFx decode(const int32_t wx[4]) {
    RemoteWolfFx f;
    f.flags = static_cast<uint8_t>(wx[0] & kWolfFxWireMask);
    if (f.flags & kWolfFxDome) {
        f.domeRadius = std::clamp(static_cast<float>(wx[1]), 0.0f, kMaxWolfDomeRadius);
    }
    f.lockDashSeq = static_cast<uint8_t>(wx[2] & 0xFF);
    f.lockCount = static_cast<uint8_t>((wx[2] >> 8) & 0xFF);
    if (f.flags & kWolfFxHairAim) {
        f.hairAim = static_cast<int16_t>(wx[3]);
    }
    return f;
}

}  // namespace twili::wolffx

