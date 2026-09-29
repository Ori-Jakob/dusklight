#pragma once

#include "presence/RemotePose.hpp"

#include "d/d_kankyo_tev_str.h"
#include "m_Do/m_Do_ext.h"

class J3DModel;
class daAlink_c;

namespace twili {

// What the autotest checks about a dummy's rod (DummyPlayerDebugInfo::fishing).
struct DummyFishingDebug {
    bool shown = false;
    uint8_t action = 0;
    float tip[3] = {};      // the rod's tip, from the dummy's own hand
    float bobber[3] = {};
    float lineLength = 0.0f;  // tip to hook along the drawn line
    uint32_t shownTicks = 0;
    uint32_t castTicks = 0;   // ticks shown with the line out (action 1)
};

// dmg_rod_class's bobber rod drawn on a dummy: never an actor, never a collider.
class DummyFishing {
public:
    static constexpr int kSegments = 15;

    struct Models {
        J3DModel* rod[kSegments] = {};
        J3DModel* uki = nullptr;
        J3DModel* ukiSaki = nullptr;
        J3DModel* hook[2] = {};
        J3DModel* esa[2] = {};
    };

    // In the dummy's createHeap (the line's buffers come from its heap too).
    bool createHeap(const Models& models);
    void clearPointers();
    // After the dummy's matrices, with the pose shown; `dummy` holds the rod in its right hand.
    void update(daAlink_c& dummy, const RemoteFishing& fr, bool shown, bool snapped);
    void draw(const dKy_tevstr_c& owner);
    const DummyFishingDebug& debug() const { return mDebug; }

private:
    void poseRod(daAlink_c& dummy, const RemoteFishing& fr, bool fresh);
    void poseBobber(const RemoteFishing& fr, const cXyz& hook);
    void poseLine(const RemoteFishing& fr, const cXyz& hook);

    Models mModels;
    bool mReady = false;
    bool mShown = false;
    bool mWasShown = false;
    uint8_t mAction = 0;
    uint8_t mHookKind = 0;
    uint8_t mEsaKind = 0;
    cXyz mJoint[kSegments + 1];
    mDoExt_3DlineMat0_c mLine;
    int mLinePoints = 0;
    dKy_tevstr_c mTevStr;
    cXyz mHookPos = cXyz::Zero;
    DummyFishingDebug mDebug;
};

}  // namespace twili
