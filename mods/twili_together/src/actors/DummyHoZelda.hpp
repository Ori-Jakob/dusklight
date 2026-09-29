#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_kankyo_tev_str.h"
#include "SSystem/SComponent/c_phase.h"
#include "m_Do/m_Do_ext.h"

#include <cstdint>

class J3DJoint;
class J3DModel;
class JKRSolidHeap;

namespace twili {

// daHoZelda_c's look behind a remote rider, from the game's HoZelda.arc and the sender's clips.
class DummyHoZelda {
public:
    // Each tick of the horse puppet; `root` is the horse's root joint.
    void update(const RemoteHorseZelda& zelda, bool wanted, MtxP root, const csXyz& angle,
                bool ridden);
    void draw(const dKy_tevstr_c& horseTev, u32 shadowId);
    void destroy();
    bool shown() const { return mShown; }
    const cXyz& pos() const { return mPos; }
    uint16_t anm() const { return mShown ? mBound[0] : 0xFFFF; }

private:
    bool build();
    void release();
    void pose(const RemoteHorseZelda& zelda, MtxP root, const csXyz& angle, bool ridden);

    request_of_phase_process_class mPhase{};
    bool mRequested = false;
    bool mFailed = false;
    JKRSolidHeap* mHeap = nullptr;
    J3DModel* mModel = nullptr;
    J3DModel* mBow = nullptr;
    mDoExt_AnmRatioPack mPacks[3];
    mDoExt_MtxCalcAnmBlendTbl* mCalc = nullptr;
    mDoExt_bckAnm mBowBck;
    uint16_t mBound[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t mBowAnm = 0xFFFF;
    float mBowFrame = 0.0f;
    bool mUpper = false;
    bool mShown = false;
    uint32_t mAbsentTicks = 0;
    cXyz mPos = cXyz::Zero;
    dKy_tevstr_c mTevStr;

    friend int dummyHoZeldaJointCallBack(J3DJoint* joint, int timing);
};

}  // namespace twili
