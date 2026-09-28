#pragma once

#include "presence/RemotePose.hpp"

#include "d/actor/d_a_player.h"
#include "d/d_kankyo_tev_str.h"
#include "JSystem/J3DGraphBase/J3DMatBlock.h"
#include "m_Do/m_Do_ext.h"

class J3DAnmTevRegKey;
class J3DAnmTexPattern;
class J3DAnmTextureSRTKey;
class J3DAnmTransform;
class J3DModel;
class J3DModelData;

namespace twili {

// Hair scale like daMidna_McaMorfCB1_c, which asks the local Midna whether to scale.
class DummyMidnaHairCB : public mDoExt_McaMorfCallBack1_c {
public:
    int execute(u16 jnt, J3DTransformInfo* info) override;
    const cXyz* mScale = nullptr;  // nullptr: FLG0_NO_HAIR_SCALE on the sender
};

class DummyMidna {
public:
    // Wmdl models as changeWolf builds them for the local wolf.
    struct Models {
        J3DModelData* body;
        J3DModel* mask;
        J3DModel* hands;
        J3DModel* hairHand;
        J3DAnmTevRegKey* tiredBrk[4];
    };

    // Forgets every pointer, without freeing
    void clearPointers();
    // Inside daDummyPlayer_c::createHeapImpl.
    bool createHeap(const Models& models);
    // initializeShell
    void initHeaps();
    void destroyHeaps();
    void deactivate() { mActive = false; }
    // No morph from the pose shown before (a snap or a form change).
    void resetContinuity() { mFresh = true; }
    // Once per tick after the wolf body's calc
    void update(const RemoteMidnaPose& pose, J3DModel* wolf, float frameAlpha);
    void draw(const dKy_tevstr_c& owner, bool ownerTinted);
    void addRealShadow(u32 shadowId);
    // daMidna_c::baseModelCallBack for md.bmd's joints (calc timing 0).
    void jointCallBack(u16 jnt);

    J3DModel* bodyModel() const;
    bool ready() const { return mReady; }
    uint8_t mode() const { return mActive ? mMode : kMidnaNone; }
    uint16_t bodyId() const { return mBodyId; }
    uint16_t upperId() const { return mShownUpperId; }
    uint8_t hairHand() const { return mHairHand; }
    uint8_t leftHand() const { return mLeftHand; }
    uint8_t rightHand() const { return mRightHand; }
    bool tired() const { return mTired; }
    uint32_t refused() const { return mRefusedCount; }
    // From her root to the wolf's joint WL_JNT_MD, 0 before the first update.
    float backDist() const { return mBackDist; }
    // The hair hand turned toward the remote's lock target on the last update, by this angle.
    bool hairAimApplied() const { return mActive && mHairAimApplied; }
    int16_t hairAimAngle() const { return mHairAimAngle; }

private:
    // Whether `id` may be read into a slot
    bool accept(uint16_t id, bool allowed, uint16_t& refused);
    void refuse(uint16_t id, uint16_t& refused);
    // True when the body clip changed.
    bool bindBody(uint16_t id, bool fresh);
    J3DAnmTransform* bindLayer(daPy_anmHeap_c& heap, uint16_t id, bool face,
                               uint16_t& boundId, J3DAnmTransform*& bound, uint16_t& refused);
    void bindTexture(bool btp, uint16_t id);
    void updateFaceTextures(float animFrame);
    void swapUpper();
    void swapFace();
    void updateParts(const RemoteMidnaPose& pose);
    void setHandShapes(uint8_t left, uint8_t right);
    void setTired(bool tired);
    void initHairColours();
    void chaseHairColours(bool big);

    mDoExt_McaMorfSO* mpMorf = nullptr;
    DummyMidnaHairCB mHairCB;
    J3DModel* mpMask = nullptr;
    J3DModel* mpHands = nullptr;
    J3DModel* mpHairHand = nullptr;
    J3DAnmTevRegKey* mTiredBrk[4] = {};
    daPy_anmHeap_c mBodyHeap;
    daPy_anmHeap_c mUpperHeap;
    daPy_anmHeap_c mFaceHeap;
    daPy_anmHeap_c mBtpHeap;
    daPy_anmHeap_c mBtkHeap;
    J3DAnmTransform* mBodyBck = nullptr;
    J3DAnmTransform* mUpperBck = nullptr;
    J3DAnmTransform* mFaceBck = nullptr;
    J3DAnmTexPattern* mpBtp = nullptr;
    J3DAnmTextureSRTKey* mpBtk = nullptr;
    // What each heap holds
    uint16_t mBodyId = 0, mUpperId = 0, mFaceId = 0, mBtpId = 0, mBtkId = 0;
    uint16_t mShownUpperId = 0, mShownFaceId = 0;
    // The last id each slot refused, so it is neither read nor counted again.
    uint16_t mRefusedBody = 0, mRefusedUpper = 0, mRefusedFace = 0, mRefusedBtp = 0,
             mRefusedBtk = 0;
    uint32_t mRefusedCount = 0;
    // The clips changeUpperBck / changeFaceBck swap with the morf's during calc.
    J3DAnmTransform* mUpperHeld = nullptr;
    J3DAnmTransform* mFaceHeld = nullptr;
    uint16_t mFaceJoint = 4;  // MD_JNT_HEAD_e
    s16 mNeckX = 0, mNeckY = 0, mBackboneZ = 0;
    int mBlinkFrame = 0;  // daMidna_c::mFrameCounter
    J3DGXColorS10 mHairColor;
    J3DGXColor mHairK1;
    J3DGXColor mHairK2;
    dKy_tevstr_c mTevStr;
    cXyz mPos = cXyz::Zero;
    float mBackDist = 0.0f;
    uint8_t mMode = kMidnaNone;
    bool mNoShadow = false;  // kMidnaNoShadow
    uint8_t mHairHand = 0xFF;
    uint8_t mLeftHand = 0xFE, mRightHand = 0xFE;
    bool mTired = false;
    bool mHairAimApplied = false;
    int16_t mHairAimAngle = 0;
    bool mReady = false;
    bool mActive = false;
    bool mFresh = true;
};

}  // namespace twili

