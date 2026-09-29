#include "actors/DummyFishing.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_math.h"

#include <algorithm>
#include <cmath>

namespace twili {
namespace {

// dmg_rod_class's uki rod (param 0xFFFF011D: segment length 29)
constexpr f32 kSegLen = 29.0f;
constexpr f32 kRodP[DummyFishing::kSegments + 1] = {
    0.0f,        0.002915448f, 0.023323584f, 0.045553874f, 0.078717098f, 0.124999836f,
    0.15374433f, 0.18658867f,  0.22380619f,  0.2656702f,   0.31245404f,  0.36443099f,
    0.48505768f, 0.62973678f,  0.80065495f,  0.99999869f,
};
constexpr u8 kRodWd[DummyFishing::kSegments] = {15, 15, 15, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 2};
constexpr int kSubdiv = 6;
constexpr int kLinePoints = (kFishingLinePoints - 1) * kSubdiv + 1;
const GXColor kLineColor = {0xFF, 0xFF, 0x96, 0xFF};
constexpr u8 kActionReady = 0;
constexpr u8 kActionStandby = 1;
constexpr u8 kActionHit = 5;
constexpr u8 kActionCatch = 6;

s16 yawOf(const cXyz& v) {
    return static_cast<s16>(cM_atan2s(v.x, v.z));
}

s16 pitchOf(const cXyz& v) {
    return static_cast<s16>(-cM_atan2s(v.y, std::sqrt(v.x * v.x + v.z * v.z)));
}

cXyz along(s16 yaw, s16 pitch, f32 len) {
    mDoMtx_stack_c::YrotS(yaw);
    mDoMtx_stack_c::XrotM(pitch);
    const Vec forward = {0.0f, 0.0f, len};
    cXyz out;
    mDoMtx_stack_c::multVec(&forward, &out);
    return out;
}

cXyz catmullRom(const cXyz& p0, const cXyz& p1, const cXyz& p2, const cXyz& p3, f32 t) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
               (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
           0.5f;
}

}  // namespace

bool DummyFishing::createHeap(const Models& models) {
    clearPointers();
    for (J3DModel* m : models.rod) {
        if (m == nullptr) {
            return false;
        }
    }
    if (models.uki == nullptr || models.ukiSaki == nullptr || models.hook[0] == nullptr ||
        models.hook[1] == nullptr || models.esa[0] == nullptr || models.esa[1] == nullptr)
    {
        return false;
    }
    if (!mLine.init(1, kLinePoints, 1)) {
        return false;
    }
    f32* size = mLine.getSize(0);
    for (int i = 0; i < kLinePoints; i++) {
        size[i] = 0.15f;
    }
    mModels = models;
    dKy_tevstr_init(&mTevStr, -1, 0xFF);
    mReady = true;
    return true;
}

void DummyFishing::clearPointers() {
    mModels = Models{};
    mReady = false;
    mShown = false;
    mWasShown = false;
}

// rod_control's uki branch, from this dummy's own hand
void DummyFishing::poseRod(daAlink_c& dummy, const RemoteFishing& fr, bool fresh) {
    mDoMtx_stack_c::copy(dummy.getRightItemMatrix());
    mDoMtx_stack_c::YrotM(-17000);
    mDoMtx_stack_c::XrotM(static_cast<s16>(0xA134));
    mDoMtx_stack_c::transM(0.0f, -1.0f, -10.0f);
    cXyz root;
    mDoMtx_stack_c::multVecZero(&root);
    mDoMtx_stack_c::transM(0.0f, 0.0f, 16.0f * kSegLen);
    cXyz aimPos;
    mDoMtx_stack_c::multVecZero(&aimPos);
    const cXyz aim = aimPos - root;
    const s16 aimYaw = yawOf(aim);
    const s16 aimPitch = pitchOf(aim);
    const cXyz stiff = along(aimYaw, aimPitch, 300.0f);
    s16 bendYaw;
    s16 bendPitch;
    if (fr.aimDown) {
        bendYaw = dummy.getFishingRodAngleY();
        bendPitch = 6000;
    } else {
        const cXyz toLine = cXyz(fr.bendTarget[0], fr.bendTarget[1], fr.bendTarget[2]) - aimPos;
        bendYaw = yawOf(toLine);
        bendPitch = pitchOf(toLine);
    }
    const cXyz bend = along(bendYaw, bendPitch, fr.bend * 3.0f);
    const f32 seg = kSegLen * fr.extend;
    mJoint[0] = root;
    if (fresh) {
        const cXyz step = along(aimYaw, aimPitch, seg);
        for (int i = 1; i <= kSegments; i++) {
            mJoint[i] = root + step * static_cast<f32>(i);
        }
    }
    for (int i = 1; i <= kSegments; i++) {
        const cXyz v = stiff + (mJoint[i] - mJoint[i - 1]) + bend * kRodP[i];
        mJoint[i] = mJoint[i - 1] + along(yawOf(v), pitchOf(v), seg);
    }
    for (int i = 0; i < kSegments; i++) {
        const cXyz d = mJoint[i + 1] - mJoint[i];
        mDoMtx_stack_c::transS(mJoint[i].x, mJoint[i].y, mJoint[i].z);
        mDoMtx_stack_c::YrotM(yawOf(d));
        mDoMtx_stack_c::XrotM(pitchOf(d));
        const f32 w = kRodWd[i] * 0.05166f;
        mDoMtx_stack_c::scaleM(w, w, 0.073f * d.abs());
        if (i == 0) {
            mDoMtx_stack_c::scaleM(1.0f, 1.0f, 0.5f);
            mDoMtx_stack_c::transM(0.0f, 0.0f, -5.5f);
        }
        mModels.rod[i]->setBaseTRMtx(mDoMtx_stack_c::get());
    }
}

// uki_main's bobber, hook and bait matrices
void DummyFishing::poseBobber(const RemoteFishing& fr, const cXyz& hook) {
    const int16_t* a = fr.bobberAng;
    mDoMtx_stack_c::transS(fr.bobber[0], fr.bobber[1], fr.bobber[2]);
    mDoMtx_stack_c::YrotM(a[0]);
    mDoMtx_stack_c::XrotM(a[1]);
    mDoMtx_stack_c::YrotM(a[2]);
    mDoMtx_stack_c::XrotM(a[3]);
    mDoMtx_stack_c::ZrotM(a[4]);
    mDoMtx_stack_c::XrotM(static_cast<s16>(a[5] * cM_ssin(a[3])));
    mDoMtx_stack_c::scaleM(0.7f, 0.7f, 0.7f);
    mDoMtx_stack_c::transM(0.0f, 0.0f, -7.0f);
    mModels.uki->setBaseTRMtx(mDoMtx_stack_c::get());
    mDoMtx_stack_c::transM(0.0f, 0.0f, 35.0f);
    mDoMtx_stack_c::scaleM(0.8f, 0.8f, 1.5f);
    mModels.ukiSaki->setBaseTRMtx(mDoMtx_stack_c::get());

    mDoMtx_stack_c::transS(hook.x, hook.y, hook.z);
    mDoMtx_stack_c::XrotM(fr.hookAng[0]);
    mDoMtx_stack_c::YrotM(fr.hookAng[1]);
    mDoMtx_stack_c::ZrotM(fr.hookAng[2]);
    mDoMtx_stack_c::push();
    mDoMtx_stack_c::XrotM(0x4000);
    const f32 hookSize = mHookKind == 1 ? 0.5f : 1.0f;
    mDoMtx_stack_c::scaleM(hookSize, hookSize, hookSize);
    mModels.hook[mHookKind]->setBaseTRMtx(mDoMtx_stack_c::get());
    mDoMtx_stack_c::pop();
    if (mEsaKind == 1) {
        mDoMtx_stack_c::XrotM(-0x8000);
        mDoMtx_stack_c::transM(0.0f, -0.5f, mHookKind == 1 ? 16.5f : 9.0f);
        mDoMtx_stack_c::scaleM(0.1f, 0.1f, 0.1f);
        mModels.esa[0]->setBaseTRMtx(mDoMtx_stack_c::get());
    } else if (mEsaKind == 2) {
        mDoMtx_stack_c::transM(0.0f, -1.0f, mHookKind == 1 ? -9.0f : -3.0f);
        mDoMtx_stack_c::scaleM(0.065f, 0.065f, 0.065f);
        mModels.esa[1]->setBaseTRMtx(mDoMtx_stack_c::get());
    }
}

// The sender's line samples, pinned to our tip and hook, smoothed into the line strip
void DummyFishing::poseLine(const RemoteFishing& fr, const cXyz& hook) {
    cXyz p[kFishingLinePoints];
    for (int i = 0; i < kFishingLinePoints; i++) {
        p[i].set(fr.line[i][0], fr.line[i][1], fr.line[i][2]);
    }
    // Our tip leads the sender's: the first samples follow it by less and less.
    const cXyz shift = mJoint[kSegments] - p[0];
    for (int i = 0; i < kFishingLinePoints - 1; i++) {
        const f32 k = 1.0f - static_cast<f32>(i) / (kFishingLinePoints - 1);
        p[i] += shift * (k * k);
    }
    p[kFishingLinePoints - 1] = hook;
    cXyz* out = mLine.getPos(0);
    f32 length = 0.0f;
    int n = 0;
    for (int i = 0; i < kFishingLinePoints - 1; i++) {
        const cXyz& p0 = p[i > 0 ? i - 1 : 0];
        const cXyz& p3 = p[i + 2 < kFishingLinePoints ? i + 2 : kFishingLinePoints - 1];
        for (int s = 0; s < kSubdiv; s++) {
            out[n] = catmullRom(p0, p[i], p[i + 1], p3, static_cast<f32>(s) / kSubdiv);
            if (n > 0) {
                length += (out[n] - out[n - 1]).abs();
            }
            n++;
        }
    }
    out[n] = p[kFishingLinePoints - 1];
    length += (out[n] - out[n - 1]).abs();
    mLinePoints = n + 1;
    mDebug.lineLength = length;
}

void DummyFishing::update(daAlink_c& dummy, const RemoteFishing& fr, bool shown, bool snapped) {
    mShown = mReady && shown && fr.active;
    mDebug.shown = mShown;
    if (!mShown) {
        mWasShown = false;
        return;
    }
    const bool fresh = !mWasShown || snapped;
    mWasShown = true;
    mAction = fr.action;
    mHookKind = fr.hookKind;
    mEsaKind = fr.esaKind;
    poseRod(dummy, fr, fresh);
    // uki_ready: the hook is in Link's left hand
    if (fr.action == kActionReady) {
        mDoMtx_multVecZero(dummy.getLeftItemMatrix(), &mHookPos);
    } else {
        mHookPos.set(fr.hook[0], fr.hook[1], fr.hook[2]);
    }
    poseBobber(fr, mHookPos);
    poseLine(fr, mHookPos);
    // Thin, but never thinner than a pixel or so from where we look
    const f32 camDist = dComIfGd_getView() != nullptr
                            ? mJoint[kSegments].abs(dComIfGd_getView()->lookat.eye)
                            : 500.0f;
    f32* size = mLine.getSize(0);
    const f32 width = std::clamp(camDist * 0.0005f, 0.12f, 1.2f);
    for (int i = 0; i < kLinePoints; i++) {
        size[i] = width;
    }
    mDebug.action = fr.action;
    mDebug.shownTicks++;
    if (fr.action == kActionStandby) {
        mDebug.castTicks++;
    }
    for (int k = 0; k < 3; k++) {
        mDebug.tip[k] = k == 0 ? mJoint[kSegments].x : k == 1 ? mJoint[kSegments].y
                                                                : mJoint[kSegments].z;
        mDebug.bobber[k] = fr.bobber[k];
    }
}

// dmg_rod_Draw's uki branch
void DummyFishing::draw(const dKy_tevstr_c& owner) {
    if (!mShown) {
        return;
    }
    mTevStr.room_no = owner.room_no;
    mTevStr.YukaCol = owner.YukaCol;
    g_env_light.settingTevStruct(0, &mHookPos, &mTevStr);
    const auto entry = [&](J3DModel* model) {
        g_env_light.setLightTevColorType_MAJI(model, &mTevStr);
        mDoExt_modelUpdateDL(model);
    };
    entry(mModels.uki);
    entry(mModels.ukiSaki);
    if (mLinePoints > 1) {
        mLine.update(mLinePoints, kLineColor, &mTevStr);
        dComIfGd_set3DlineMat(&mLine);
    }
    for (J3DModel* seg : mModels.rod) {
        entry(seg);
    }
    if (mAction != kActionHit && mAction != kActionCatch) {
        entry(mModels.hook[mHookKind]);
        if (mEsaKind != 0) {
            entry(mModels.esa[mEsaKind - 1]);
        }
    }
}

}  // namespace twili
