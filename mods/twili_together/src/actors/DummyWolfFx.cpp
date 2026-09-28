// The remote wolf's attack effects from PLAYER_UPDATE "wx".

#include "actors/DummyPlayer.hpp"
#include "fx/WolfFx.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_particle_name.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/AlAnm.h"
#include "res/Object/Wmdl.h"

namespace twili {

// A sequence step past this is a restart (reconnect, new stream), not bursts we missed.
static constexpr uint8_t kMaxLockDashCatchUp = 3;

// setWolfLockDomeModel without what reaches past this actor
bool daDummyPlayer_c::loadRemoteLockDome() {
    JKRHeap* prev = setItemHeap();
    J3DModelData* data = loadAramBmd(dRes_ID_ALANM_BMD_S_BALL_YAMI_e, 0x1C00);
    mHeldItemModel = data != nullptr ? initModel(data, 0x200) : nullptr;
    if (mHeldItemModel != nullptr) {
        field_0x0718 = loadAramItemBtk(dRes_ID_ALANM_BTK_S_BALL_YAMI_e, mHeldItemModel);
        field_0x0724 = loadAramItemBrk(dRes_ID_ALANM_BRK_S_BALL_YAMI_e, mHeldItemModel);
    }
    mDoExt_setCurrentHeap(prev);
    if (mHeldItemModel == nullptr) {
        return false;
    }
    mEquipItem = twili::wolffx::kLockDomeItem;
    // Our own darkness picks the colour
    field_0x0724->setFrame(dKy_darkworld_check() ? 1.0f : 0.0f);
    return true;
}

// Stops what is shown.
void daDummyPlayer_c::clearRemoteWolfFx() {
    if (mDummyWolfSpinWasActive) {
        clearCutTurnEffectID();
        mDummyWolfSpinWasActive = false;
    }
    for (u32& id : field_0x31b0) {
        stopDrawParticle(id);
        id = 0;
    }
    mDummyLockBlurAlpha = 0;
}

// After the body's calc
void daDummyPlayer_c::updateRemoteWolfFx(const LinkPuppetState& state) {
    const RemoteWolfFx& fx = state.wolfFx;
    const bool wolf = checkWolf() != 0;
    const bool shown = !isHidden();

    // One LOCKATDASH per new jump.
    if (!mDummyLockDashSeqValid) {
        mDummyLockDashSeq = fx.lockDashSeq;
        mDummyLockDashSeqValid = true;
    } else if (fx.lockDashSeq != mDummyLockDashSeq) {
        const uint8_t ahead = static_cast<uint8_t>(fx.lockDashSeq - mDummyLockDashSeq);
        if (shown && wolf && ahead <= kMaxLockDashCatchUp) {
            // procWolfLockAttackInit fires it where the jump starts, before that tick's move
            JPABaseEmitter* burst =
                dComIfGp_particle_set(ID_ZI_J_WL_LOCKATDASH_A, &old.pos, &current.angle, nullptr);
            if (burst != nullptr) {
                mDummyLockDashes++;
            }
        }
        mDummyLockDashSeq = fx.lockDashSeq;
    }

    if (!shown || !wolf) {
        clearRemoteWolfFx();
        return;
    }

    if (fx.flags & twili::kWolfFxSpin) {
        const bool right = (fx.flags & twili::kWolfFxSpinRight) != 0;
        mProcVar2.field_0x300c = right ? 1 : 0;
        mProcVar3.field_0x300e = (fx.flags & twili::kWolfFxSpinNoTrail) ? 1 : 0;
        setWolfRollAttackEffect();
        mDummyWolfSpinWasActive = true;
        mDummyWolfSpinTicks++;
        mDummyWolfLastSpin = right ? 2 : 1;
        mDummyWolfSpinEmitters = 0;
        for (int i = 0; i < 2; i++) {
            if (dComIfGp_particle_getEmitter(field_0x3204[i]) != nullptr) {
                mDummyWolfSpinEmitters++;
            }
        }
    } else if (mDummyWolfSpinWasActive) {
        clearCutTurnEffectID();
        mDummyWolfSpinWasActive = false;
    }

    updateRemoteWolfLockBlur((fx.flags & twili::kWolfFxLockBlur) != 0);
}

void daDummyPlayer_c::updateRemoteWolfLockBlur(bool active) {
    static const u16 effID[] = {
        ID_ZI_J_WL_LOCKATBLUR_A,
        ID_ZI_J_WL_LOCKATBLUR_B,
        ID_ZI_J_WL_LOCKATBLUR_IND,
    };
    u8 alpha = 0;
    if (active) {
        alpha = 0xFF;
    } else if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(field_0x31b0[0])) {
        alpha = emitter->getGlobalAlpha();
        alpha = alpha >= 51 ? static_cast<u8>(alpha - 51) : 0;
    }
    mDummyLockBlurAlpha = alpha;
    if (alpha == 0) {
        return;
    }
    cXyz pos;
    mDoMtx_multVecZero(mpLinkModel->getAnmMtx(WL_JNT_TAIL1_e), &pos);
    for (int i = 0; i < 3; i++) {
        if (JPABaseEmitter* emitter = setEmitter(&field_0x31b0[i], effID[i], &pos, &shape_angle)) {
            emitter->setGlobalAlpha(alpha);
        }
    }
}

}  // namespace twili
