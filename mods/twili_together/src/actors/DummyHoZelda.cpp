#include "actors/DummyHoZelda.hpp"

#include "core/Log.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_math.h"
#include "SSystem/SComponent/c_phase.h"

#include <algorithm>

namespace twili {
namespace {

constexpr const char* kArc = "HoZelda";
constexpr u16 kZeldaBmd = 0x23;
constexpr u16 kBowBmd = 0x20;
constexpr u16 kBowIdleBck = 0xC;
constexpr u16 kJointNum = 47;
constexpr u16 kUpperEndJoint = 23;
constexpr u32 kHeapSize = 0x10000;
// She goes back to the archive after this long without the sender's Zelda.
constexpr uint32_t kReleaseTicks = 150;

// daHoZelda_c::setMatrix and setRideOffset, behind a rider or alone in the saddle
const Vec kRidePos = {-5.894f, 52.61f, 4.079f};
const Vec kFrontRidePos = {-75.893997f, 57.61f, 4.079f};
const Vec kRideOffset = {0.1f, 236.7f, -63.554f};
const Vec kFrontRideOffset = {0.1f, 241.7f, 6.445999f};

J3DAnmTransform* clip(uint16_t id) {
    if (id == 0xFFFF) {
        return nullptr;
    }
    auto* anm = static_cast<J3DAnmBase*>(dComIfG_getObjectRes(kArc, id));
    const s32 kind = anm != nullptr ? anm->getKind() : -1;
    return kind == 0 || kind == 8 || kind == 9 ? static_cast<J3DAnmTransform*>(anm) : nullptr;
}

}  // namespace

// daHoZelda_c::modelCallBack's upper-body switch (joints 1-22 while the bow is up).
int dummyHoZeldaJointCallBack(J3DJoint* joint, int timing) {
    if (timing == 0) {
        auto* self = reinterpret_cast<DummyHoZelda*>(j3dSys.getModel()->getUserArea());
        if (joint->getJntNo() == 0 && self->mUpper) {
            self->mPacks[2].setRatio(1.0f);
        } else if (joint->getJntNo() == kUpperEndJoint) {
            self->mPacks[2].setRatio(0.0f);
        }
    }
    return 1;
}

bool DummyHoZelda::build() {
    auto* data = static_cast<J3DModelData*>(dComIfG_getObjectRes(kArc, kZeldaBmd));
    auto* bowData = static_cast<J3DModelData*>(dComIfG_getObjectRes(kArc, kBowBmd));
    J3DAnmTransform* bowIdle = clip(kBowIdleBck);
    if (data == nullptr || bowData == nullptr || bowIdle == nullptr ||
        data->getJointNum() != kJointNum)
    {
        return false;
    }
    mHeap = mDoExt_createSolidHeapFromGame(kHeapSize, 0x20);
    if (mHeap == nullptr) {
        return false;
    }
    JKRHeap* prev = mDoExt_setCurrentHeap(mHeap);
    mModel = mDoExt_J3DModel__create(data, 0, 0x11020284);
    mBow = mDoExt_J3DModel__create(bowData, 0x80000, 0x11000084);
    mCalc = JKR_NEW mDoExt_MtxCalcAnmBlendTbl(3, mPacks);
    const bool bck = mBowBck.init(bowIdle, 1, 0, 1.0f, 0, -1, false);
    mDoExt_setCurrentHeap(prev);
    mDoExt_adjustSolidHeap(mHeap);
    if (mModel == nullptr || mBow == nullptr || mCalc == nullptr || !bck) {
        release();
        return false;
    }
    mModel->setUserArea(reinterpret_cast<uintptr_t>(this));
    mBowAnm = kBowIdleBck;
    dKy_tevstr_init(&mTevStr, -1, 0xFF);
    return true;
}

void DummyHoZelda::release() {
    if (mHeap != nullptr) {
        mDoExt_destroySolidHeap(mHeap);
        mHeap = nullptr;
    }
    mModel = mBow = nullptr;
    mCalc = nullptr;
    for (mDoExt_AnmRatioPack& pack : mPacks) {
        pack.setAnmTransform(nullptr);
        pack.setRatio(0.0f);
    }
    for (uint16_t& id : mBound) {
        id = 0xFFFF;
    }
    mBowAnm = 0xFFFF;
    mShown = false;
    if (mRequested) {
        dComIfG_resDelete(&mPhase, kArc);
        mRequested = false;
    }
}

void DummyHoZelda::update(const RemoteHorseZelda& zelda, bool wanted, MtxP root,
                          const csXyz& angle, bool ridden) {
    mShown = false;
    if (!wanted) {
        if (mRequested && ++mAbsentTicks > kReleaseTicks) {
            release();
        }
        return;
    }
    mAbsentTicks = 0;
    if (mModel == nullptr) {
        if (mFailed) {
            return;
        }
        // As daHoZelda_c::create: the archive is the game's, counted per request.
        mRequested = true;
        const int phase = dComIfG_resLoad(&mPhase, kArc);
        if (phase == cPhs_ERROR_e) {
            mFailed = true;
            mRequested = false;
            TwiliLog.warn("[horse] HoZelda.arc did not load; the remote Zelda is not shown");
            return;
        }
        if (phase != cPhs_COMPLEATE_e) {
            return;
        }
        if (!build()) {
            mFailed = true;
            TwiliLog.warn("[horse] the remote Zelda's models could not be built");
            return;
        }
    }
    pose(zelda, root, angle, ridden);
}

void DummyHoZelda::pose(const RemoteHorseZelda& zelda, MtxP root, const csXyz& angle,
                        bool ridden) {
    for (int i = 0; i < 3; i++) {
        J3DAnmTransform* anm = clip(zelda.anm[i]);
        if (anm != nullptr && anm->getFrameMax() > 0) {
            anm->setFrame(std::clamp(zelda.frame[i], 0.0f, static_cast<f32>(anm->getFrameMax())));
        }
        mPacks[i].setAnmTransform(anm);
        mBound[i] = anm != nullptr ? zelda.anm[i] : 0xFFFF;
    }
    if (mPacks[0].getAnmTransform() == nullptr) {
        return;
    }
    const bool second = mPacks[1].getAnmTransform() != nullptr;
    mPacks[0].setRatio(second ? 1.0f - zelda.ratio : 1.0f);
    mPacks[1].setRatio(second ? zelda.ratio : 0.0f);
    mPacks[2].setRatio(0.0f);
    mUpper = zelda.upper && mPacks[2].getAnmTransform() != nullptr;

    mDoMtx_multVec(root, ridden ? &kRidePos : &kFrontRidePos, &mPos);
    const Vec& offset = ridden ? kRideOffset : kFrontRideOffset;
    mDoMtx_stack_c::transS(mPos);
    mDoMtx_stack_c::ZXYrotM(angle.x, angle.y, angle.z);
    mDoMtx_stack_c::transM(-offset.x, -offset.y, -offset.z);
    mModel->setBaseTRMtx(mDoMtx_stack_c::get());

    // The model data is the game's: another daHoZelda_c may have hooked its joints.
    J3DModelData* data = mModel->getModelData();
    J3DJoint* rootJoint = data->getJointNodePointer(0);
    static constexpr u16 kHooked[] = {0, 1, 4, kUpperEndJoint};
    J3DJointCallBack saved[4];
    for (int i = 0; i < 4; i++) {
        J3DJoint* j = data->getJointNodePointer(kHooked[i]);
        saved[i] = j->getCallBack();
        j->setCallBack(kHooked[i] == 0 || kHooked[i] == kUpperEndJoint ? dummyHoZeldaJointCallBack
                                                                        : nullptr);
    }
    J3DMtxCalc* savedCalc = rootJoint->getMtxCalc();
    rootJoint->setMtxCalc(mCalc);
    mModel->calc();
    rootJoint->setMtxCalc(savedCalc);
    for (int i = 0; i < 4; i++) {
        data->getJointNodePointer(kHooked[i])->setCallBack(saved[i]);
    }

    // setBowModel
    if (zelda.bowAnm != mBowAnm) {
        if (J3DAnmTransform* bck = clip(zelda.bowAnm)) {
            mBowBck.init(bck, 1, -1, 1.0f, 0, -1, true);
            mBowAnm = zelda.bowAnm;
        }
    }
    mBowFrame = zelda.bowFrame;
    mDoMtx_stack_c::copy(mModel->getAnmMtx(0x11));
    mDoMtx_stack_c::transM(10.0f, -2.0f, 0.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(95.0f), 0, cM_deg2s(10.0f));
    mBow->setBaseTRMtx(mDoMtx_stack_c::get());
    mBowBck.entry(mBow->getModelData(), mBowFrame);
    mBow->calc();
    mShown = true;
}

// daHoZelda_c::draw
void DummyHoZelda::draw(const dKy_tevstr_c& horseTev, u32 shadowId) {
    if (!mShown) {
        return;
    }
    mTevStr.room_no = horseTev.room_no;
    mTevStr.YukaCol = horseTev.YukaCol;
    g_env_light.settingTevStruct(0, &mPos, &mTevStr);
    g_env_light.setLightTevColorType_MAJI(mModel, &mTevStr);
    mDoExt_modelEntryDL(mModel);
    g_env_light.setLightTevColorType_MAJI(mBow, &mTevStr);
    mDoExt_modelEntryDL(mBow);
    if (shadowId != 0) {
        dComIfGd_addRealShadow(shadowId, mModel);
        dComIfGd_addRealShadow(shadowId, mBow);
    }
}

void DummyHoZelda::destroy() {
    release();
    mFailed = false;
}

}  // namespace twili
