#include "actors/DummyMidna.hpp"

#include "core/Host.hpp"
#include "core/Log.hpp"
#include "presence/RemoteFrame.hpp"

#include "d/actor/d_a_midna.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_stage.h"
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/AlAnm.h"
#include "res/Object/Wmdl.h"
#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <algorithm>

namespace twili {
namespace {

constexpr u16 kJointNum = MD_JNT_FOOT_R_e + 1;
// daMidna_c::createHeap's clip buffers
constexpr u32 kBodyBufferSize = 0x3800;
constexpr u32 kFaceBufferSize = 0x1000;
constexpr u32 kTexBufferSize = 0x400;
constexpr f32 kMorfFrames = 5.0f;  // daMidna_c::setAnm's morph into a new clip
constexpr u16 kIdleBck = dRes_ID_ALANM_BCK_MD_WAITA_e;
// m_texDataTable[0], the blinking face
constexpr u16 kBlinkBtp = dRes_ID_ALANM_BTP_MD_MABA01_e;
constexpr u16 kBlinkBtk = dRes_ID_ALANM_BTK_MD_MABA01_e;
constexpr f32 kBlinkChance = 0.012f;  // allAnimePlay
constexpr s32 kKindTexPattern = 2;
constexpr s32 kKindTexSrt = 4;
// Her sounds are only started this near the local player (the dummy's own limit).
constexpr f32 kAudibleDistanceSq = 3500.0f * 3500.0f;

// File statics of d_a_midna.cpp
const cXyz kHairScale[5] = {
    cXyz(0.3f, 0.8f, 0.7f), cXyz(0.2f, 0.8f, 0.4f), cXyz(0.15f, 0.75f, 0.5f),
    cXyz(0.1f, 0.7f, 0.7f), cXyz(1.0f, 1.0f, 1.0f),
};
const GXColorS10 kNormalColor = {0x50, 0x00, 0x00, 0x00};
const GXColor kNormalKColor = {0xB4, 0x87, 0x00, 0x00};
const GXColor kNormalKColor2 = {0x00, 0xC3, 0xC3, 0x00};
const GXColorS10 kBigColor = {0xFF, 0x64, 0x78, 0x00};
const GXColor kBigKColor = {0x1E, 0x00, 0x00, 0x00};
const GXColor kLightNormalKColor = {0xFF, 0xDC, 0x00, 0x00};
const GXColor kLightNormalKColor2 = {0x00, 0xC3, 0xEB, 0x00};
const GXColorS10 kLightBigColor = {0xFF, 0x78, 0x00, 0x00};
const GXColor kLightBigKColor2 = {0xAA, 0xFF, 0xC3, 0x00};

// Only what daMidna_c reads into that kind of heap (setAnm, setUpperAnime, setFaceAnime)
bool isMidnaClip(u16 id, bool face) {
    const int first = face ? daMidna_c::ANM_FTALKA : daMidna_c::ANM_WAITA;
    const int last = face ? daMidna_c::ANM_MAX : daMidna_c::ANM_FTALKA;
    for (int i = first; i < last; i++) {
        if (daMidna_c::m_anmDataTable[i].mResID == id) {
            return true;
        }
    }
    return false;
}

bool isMidnaTex(u16 id, bool btp) {
    for (const daMidna_texData_s& tex : daMidna_c::m_texDataTable) {
        if ((btp ? tex.mBtpID : tex.mBtkID) == id) {
            return true;
        }
    }
    return false;
}

// The heap's previous clip is gone as soon as the read starts (setAnimeHeap frees its solid heap)
J3DAnmTransform* loadMidnaClip(daPy_anmHeap_c& heap, u16 id) {
    if (heap.getIdx() == id) {
        heap.resetIdx();  // loadDataIdx returns NULL for the clip it already names
    }
    J3DAnmBase* anm = static_cast<J3DAnmBase*>(heap.loadDataIdx(id));
    const s32 kind = anm != nullptr ? anm->getKind() : -1;
    if (kind != 0 && kind != 8 && kind != 9) {
        return nullptr;
    }
    J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(anm);
    return bck->field_0x1e == kJointNum ? bck : nullptr;  // one track per joint
}

J3DAnmBase* loadMidnaTex(daPy_anmHeap_c& heap, u16 id, s32 kind) {
    if (heap.getIdx() == id) {
        heap.resetIdx();
    }
    J3DAnmBase* anm = static_cast<J3DAnmBase*>(heap.loadDataIdx(id));
    return anm != nullptr && anm->getKind() == kind ? anm : nullptr;
}

void showShape(J3DModel* model, u16 material, bool show) {
    J3DModelData* data = model->getModelData();
    if (material >= data->getMaterialNum()) {
        return;
    }
    J3DShape* shape = data->getMaterialNodePointer(material)->getShape();
    if (shape == nullptr) {
        return;
    }
    if (show) {
        shape->show();
    } else {
        shape->hide();
    }
}

u16 materialCount(J3DModel* model) {
    return model->getModelData()->getMaterialNum();
}

bool jointsInCalcOrder(J3DJoint* joint, u16& next) {
    for (; joint != nullptr; joint = joint->getYounger()) {
        if (joint->getJntNo() != next++ || !jointsInCalcOrder(joint->getChild(), next)) {
            return false;
        }
    }
    return true;
}

void destroyAnmHeap(daPy_anmHeap_c& heap) {
    if (heap.mAnimeHeap != nullptr) {
        mDoExt_destroySolidHeap(heap.mAnimeHeap);
        heap.mAnimeHeap = nullptr;
    }
}

// daMidna_modelCallBack.
int dummyMidnaJointCallBack(J3DJoint* joint, int calcTiming) {
    if (calcTiming == 0) {  // after the joint's own calc, before its children
        reinterpret_cast<DummyMidna*>(j3dSys.getModel()->getUserArea())
            ->jointCallBack(joint->getJntNo());
    }
    return 1;
}

}  // namespace

int DummyMidnaHairCB::execute(u16 jnt, J3DTransformInfo* info) {
    if (mScale != nullptr && jnt >= MD_JNT_HAIR_1_e && jnt <= MD_JNT_HAIR_5_e) {
        const cXyz& scale = mScale[jnt - MD_JNT_HAIR_1_e];
        info->mScale.x *= scale.x;
        info->mScale.y *= scale.y;
        info->mScale.z *= scale.z;
    }
    return 1;
}

// daMidna_matAnm_c::calc
void DummyMidnaEyeAnm::calc(J3DMaterial* material) const {
    J3DMaterialAnm::calc(material);
    const uint8_t flags = mFlags != nullptr ? *mFlags : 0;
    const uint8_t morf = (flags >> kMidnaEyeMorfShift) & 7;
    for (u32 i = 0; i < 8; i++) {
        if (!getTexMtxAnm(i).getAnmFlag()) {
            continue;
        }
        J3DTexMtxInfo& info = material->getTexGenBlock()->getTexMtx(i)->getTexMtxInfo();
        if (morf != 0) {
            const f32 t = 1.0f / (morf + 1);
            info.mSRT.mTranslationX = mOldX * (1.0f - t) + info.mSRT.mTranslationX * t;
            info.mSRT.mTranslationY = mOldY * (1.0f - t) + info.mSRT.mTranslationY * t;
        } else if (flags & kMidnaEyeMove) {
            info.mSRT.mTranslationX = mNowX;
            info.mSRT.mTranslationY = mNowY;
        }
        mOldX = info.mSRT.mTranslationX;
        mOldY = info.mSRT.mTranslationY;
    }
}

J3DModel* DummyMidna::bodyModel() const {
    return mpMorf != nullptr ? mpMorf->getModel() : nullptr;
}

void DummyMidna::clearPointers() {
    mpMorf = nullptr;
    mHairCB.mScale = nullptr;
    mpMask = mpHands = mpHairHand = nullptr;
    mEyeAnm[0] = mEyeAnm[1] = nullptr;
    mEyeFlags = 0;
    mBasBound = false;
    for (J3DAnmTevRegKey*& brk : mTiredBrk) {
        brk = nullptr;
    }
    // daPy_anmHeap_c's constructor leaves these unset.
    for (daPy_anmHeap_c* heap : {&mBodyHeap, &mUpperHeap, &mFaceHeap, &mBtpHeap, &mBtkHeap}) {
        heap->initData();
        heap->mBuffer = nullptr;
        heap->mAnimeHeap = nullptr;
    }
    mBodyBck = mUpperBck = mFaceBck = nullptr;
    mpBtp = nullptr;
    mpBtk = nullptr;
    mBodyId = mUpperId = mFaceId = mBtpId = mBtkId = 0;
    mShownUpperId = mShownFaceId = 0;
    mRefusedBody = mRefusedUpper = mRefusedFace = mRefusedBtp = mRefusedBtk = 0;
    mRefusedCount = 0;
    mUpperHeld = mFaceHeld = nullptr;
    mBackDist = 0.0f;
    mMode = kMidnaNone;
    mHairHand = 0xFF;
    mLeftHand = mRightHand = 0xFE;
    mTired = mReady = mActive = false;
    mFresh = true;
}

bool DummyMidna::createHeap(const Models& models) {
    clearPointers();
    if (models.body == nullptr || models.mask == nullptr || models.hands == nullptr ||
        models.hairHand == nullptr || models.body->getJointNum() != kJointNum ||
        models.body->getMaterialNum() < 8 || materialCount(models.hairHand) < 3)
    {
        return false;
    }
    // daMidna_c's own pose machinery, straight on md.bmd
    mpMorf = JKR_NEW mDoExt_McaMorfSO(models.body, &mHairCB, NULL, NULL,
                                      J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, NULL, 0,
                                      0x11020284);
    if (mpMorf == nullptr || mpMorf->getModel() == nullptr) {
        mpMorf = nullptr;
        return false;
    }
    J3DModel* md = mpMorf->getModel();
    J3DModelData* data = md->getModelData();
    u16 next = 0;
    if (!jointsInCalcOrder(data->getJointNodePointer(0), next) || next != kJointNum) {
        TwiliLog.warn("[dummy] Midna: md.bmd does not calc its joints in index order; her upper "
                     "and face layers will cover other joints");
    }
    md->setUserArea(reinterpret_cast<uintptr_t>(this));
    static const u16 kCallBackJoints[] = {
        MD_JNT_WORLD_ROOT_e, MD_JNT_BACKBONE1_e, MD_JNT_BACKBONE2_e, MD_JNT_HEAD_e,
        MD_JNT_CHIN_e,       MD_JNT_MOUTH_e,     MD_JNT_HAND_R_e,
    };
    for (u16 jnt : kCallBackJoints) {
        data->getJointNodePointer(jnt)->setCallBack(dummyMidnaJointCallBack);
    }

    // initMidnaModel: her eyes follow the sender's offsets, not the local Midna's statics.
    for (int i = 0; i < 2; i++) {
        mEyeAnm[i] = JKR_NEW DummyMidnaEyeAnm();
        if (mEyeAnm[i] == nullptr) {
            return false;
        }
        mEyeAnm[i]->mFlags = &mEyeFlags;
        data->getMaterialNodePointer(static_cast<u16>(2 + i))->setMaterialAnm(mEyeAnm[i]);
    }

    mpMask = models.mask;
    mpHands = models.hands;
    mpHairHand = models.hairHand;
    // initMidnaModel
    showShape(md, 6, true);
    showShape(md, 7, true);
    for (u16 i = 0; i < 4; i++) {
        showShape(mpHands, i, false);
    }
    J3DModel* owners[4] = {md, mpMask, mpHands, mpHairHand};
    for (int i = 0; i < 4; i++) {
        mTiredBrk[i] = models.tiredBrk[i];
        if (mTiredBrk[i] != nullptr) {
            mTiredBrk[i]->searchUpdateMaterialID(owners[i]->getModelData());
            mTiredBrk[i]->setFrame(1.0f);  // changeWolf
        }
    }

    mBodyHeap.setBufferSize(kBodyBufferSize);
    mUpperHeap.setBufferSize(kBodyBufferSize);
    mFaceHeap.setBufferSize(kFaceBufferSize);
    mBtpHeap.setBufferSize(kTexBufferSize);
    mBtkHeap.setBufferSize(kTexBufferSize);
    for (daPy_anmHeap_c* heap : {&mBodyHeap, &mUpperHeap, &mFaceHeap, &mBtpHeap, &mBtkHeap}) {
        if (heap->mallocBuffer() == nullptr) {
            return false;
        }
    }
    return true;
}

void DummyMidna::initHeaps() {
    if (mpMorf == nullptr) {
        return;
    }
    PLAYER_CREATE_ANM_HEAP(mBodyHeap, daPy_anmHeap_c::HEAP_TYPE_3, "DummyMidna::mBodyHeap");
    PLAYER_CREATE_ANM_HEAP(mUpperHeap, daPy_anmHeap_c::HEAP_TYPE_3, "DummyMidna::mUpperHeap");
    PLAYER_CREATE_ANM_HEAP(mFaceHeap, daPy_anmHeap_c::HEAP_TYPE_3, "DummyMidna::mFaceHeap");
    PLAYER_CREATE_ANM_HEAP(mBtpHeap, daPy_anmHeap_c::HEAP_TYPE_1, "DummyMidna::mBtpHeap");
    PLAYER_CREATE_ANM_HEAP(mBtkHeap, daPy_anmHeap_c::HEAP_TYPE_2, "DummyMidna::mBtkHeap");
    mBodyBck = loadMidnaClip(mBodyHeap, kIdleBck);
    mBodyId = mBodyBck != nullptr ? kIdleBck : 0;
    mpMorf->setAnm(mBodyBck, -1, 0.0f, 1.0f, 0.0f, -1.0f);
    bindTexture(true, kBlinkBtp);
    bindTexture(false, kBlinkBtk);
    dKy_tevstr_init(&mTevStr, -1, 0xFF);
    initHairColours();
    if (!mSoundReady) {
        mSound.init(&mPos, &mEyePos, 3, 1);
        mSoundReady = true;
    }
    mReady = mBodyBck != nullptr && mpBtp != nullptr && mpBtk != nullptr;
    if (!mReady) {
        TwiliLog.warn("[dummy] Midna: idle clip or blinking face failed to load");
    }
}

void DummyMidna::destroyHeaps() {
    if (mSoundReady) {
        mSound.deleteObject();
        mSoundReady = false;
    }
    mBasBound = false;
    for (daPy_anmHeap_c* heap : {&mBodyHeap, &mUpperHeap, &mFaceHeap, &mBtpHeap, &mBtkHeap}) {
        destroyAnmHeap(*heap);
    }
    mReady = mActive = false;
}

void DummyMidna::deactivate() {
    mActive = false;
    if (mSoundReady) {
        mSound.framework(0, dComIfGp_getReverb(dComIfGp_roomControl_getStayNo()));
    }
}

bool DummyMidna::accept(uint16_t id, bool allowed, uint16_t& refused) {
    if (id == refused) {
        return false;
    }
    if (!allowed) {
        refuse(id, refused);
        return false;
    }
    return true;
}

void DummyMidna::refuse(uint16_t id, uint16_t& refused) {
    refused = id;
    mRefusedCount++;
    TwiliLog.debug("[dummy] Midna: refused Midna anm 0x{:X}", id);
}

bool DummyMidna::bindBody(uint16_t id, bool fresh) {
    if (!accept(id, isMidnaClip(id, false), mRefusedBody)) {
        id = kIdleBck;
    }
    if (id == mBodyId && mBodyBck != nullptr) {
        if (fresh) {
            // Back after a break on the same clip
            mpMorf->setMorf(0.0f);
        }
        return false;
    }
    J3DAnmTransform* bck = loadMidnaClip(mBodyHeap, id);
    if (bck == nullptr && id != kIdleBck) {
        refuse(id, mRefusedBody);
        id = kIdleBck;
        bck = loadMidnaClip(mBodyHeap, id);
    }
    mBodyBck = bck;
    mBodyId = bck != nullptr ? id : 0;
    mpMorf->setAnm(bck, -1, fresh ? 0.0f : kMorfFrames, 1.0f, 0.0f, -1.0f);
    return true;
}

// setBckAnime: the body clip's BAS, read from the heap's copy of the file
void DummyMidna::bindSound() {
    mBasBound = false;
    u8* buf = mBodyHeap.getBuffer();
    if (!mSoundReady || mBodyBck == nullptr || buf == nullptr) {
        return;
    }
    const u32 offset = *reinterpret_cast<BE(u32)*>(buf + 0x1C);
    if (offset == 0xFFFFFFFF || offset >= kBodyBufferSize) {
        return;
    }
    mSoundFrame = mpMorf->getFrame();
    mSound.initAnime(buf + offset, true, 0.0f, mSoundFrame);
    mBasBound = true;
}

// setSound's clip sounds and the tired sigh; the rest arrives as PLAYER_SFX.
void DummyMidna::updateSound(const RemoteMidnaPose& pose, bool shown) {
    if (!mSoundReady) {
        return;
    }
    const s8 reverb = dComIfGp_getReverb(dComIfGp_roomControl_getStayNo());
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const bool heard = shown && (mMode == kMidnaDrawn || mMode == kMidnaApart) &&
                       player != nullptr && player->current.pos.abs2XZ(mPos) <= kAudibleDistanceSq;
    mSound.framework(0, reverb);
    if (!heard) {
        return;
    }
    if (mBasBound) {
        const f32 frame = mpMorf->getFrame();
        f32 rate = frame - mSoundFrame;
        if (rate < 0.0f && mBodyBck->getAttribute() == J3DFrameCtrl::EMode_LOOP) {
            rate += mBodyBck->getFrameMax();
        }
        mSoundFrame = frame;
        mSound.updateAnime(frame, std::clamp(rate, 0.0f, 4.0f));
    }
    if ((pose.flags & kMidnaTired) && mMode == kMidnaDrawn) {
        mSound.startCreatureVoiceLevel(Z2SE_MDN_V_WAITD, reverb);
    }
}

void DummyMidna::playSfx(uint32_t id, bool voice, uint32_t mapInfo, const cXyz& fallback) {
    if (!mSoundReady) {
        return;
    }
    if (!mActive) {
        mPos = fallback;
        mEyePos = fallback;
        mEyePos.y += 100.0f;
    }
    const s8 reverb = dComIfGp_getReverb(dComIfGp_roomControl_getStayNo());
    if (voice) {
        mSound.startCreatureVoice(id, reverb);
    } else {
        mSound.startCreatureSound(id, mapInfo, reverb);
    }
    mSfxPlayed++;
}

J3DAnmTransform* DummyMidna::bindLayer(daPy_anmHeap_c& heap, uint16_t id, bool face,
                                       uint16_t& boundId, J3DAnmTransform*& bound,
                                       uint16_t& refused) {
    // An unused layer keeps its clip in the heap in case it comes back.
    if (id == 0 || !accept(id, isMidnaClip(id, face), refused)) {
        return nullptr;
    }
    if (id == boundId && bound != nullptr) {
        return bound;
    }
    bound = loadMidnaClip(heap, id);
    boundId = bound != nullptr ? id : 0;
    if (bound == nullptr) {
        refuse(id, refused);
    }
    return bound;
}

// setFaceBtp / setFaceBtk on the dummy's own md.bmd data.
void DummyMidna::bindTexture(bool btp, uint16_t id) {
    uint16_t& boundId = btp ? mBtpId : mBtkId;
    const uint16_t blink = btp ? kBlinkBtp : kBlinkBtk;
    if (id == 0 || !accept(id, isMidnaTex(id, btp), btp ? mRefusedBtp : mRefusedBtk)) {
        id = blink;
    }
    J3DAnmBase* bound = btp ? static_cast<J3DAnmBase*>(mpBtp) : mpBtk;
    if (id == boundId && bound != nullptr) {
        return;
    }
    J3DModelData* data = mpMorf->getModel()->getModelData();
    // The old animator's tables live in the buffer the read overwrites
    if (btp && mpBtp != nullptr) {
        data->removeTexNoAnimator(mpBtp);
    } else if (!btp && mpBtk != nullptr) {
        data->removeTexMtxAnimator(mpBtk);
    }
    daPy_anmHeap_c& heap = btp ? mBtpHeap : mBtkHeap;
    const s32 kind = btp ? kKindTexPattern : kKindTexSrt;
    J3DAnmBase* anm = loadMidnaTex(heap, id, kind);
    if (anm == nullptr && id != blink) {
        refuse(id, btp ? mRefusedBtp : mRefusedBtk);
        id = blink;
        anm = loadMidnaTex(heap, id, kind);
    }
    if (btp) {
        mpBtp = static_cast<J3DAnmTexPattern*>(anm);
        if (mpBtp != nullptr) {
            mpBtp->searchUpdateMaterialID(data);
            data->entryTexNoAnimator(mpBtp);
            mpBtp->setFrame(0.0f);
        }
    } else {
        mpBtk = static_cast<J3DAnmTextureSRTKey*>(anm);
        if (mpBtk != nullptr) {
            mpBtk->searchUpdateMaterialID(data);
            data->entryTexMtxAnimator(mpBtk);
            mpBtk->setFrame(0.0f);
        }
    }
    boundId = anm != nullptr ? id : 0;
    mBlinkFrame = 0;
}

// allAnimePlay's face frames
void DummyMidna::updateFaceTextures(float animFrame) {
    if (mpBtp == nullptr || mpBtk == nullptr) {
        return;
    }
    int frame;
    if (mBtpId == kBlinkBtp) {
        if (mBlinkFrame != 0) {
            if (++mBlinkFrame > mpBtp->getFrameMax()) {
                mBlinkFrame = 0;
            }
        } else if (cM_rnd() < kBlinkChance) {
            mBlinkFrame++;
        }
        frame = mBlinkFrame;
    } else {
        frame = static_cast<s16>(animFrame);
    }
    mpBtp->setFrame(static_cast<f32>((std::min)(frame, static_cast<int>(mpBtp->getFrameMax()))));
    mpBtk->setFrame(static_cast<f32>((std::min)(frame, static_cast<int>(mpBtk->getFrameMax()))));
}

// changeUpperBck.
void DummyMidna::swapUpper() {
    J3DAnmTransform* held = mUpperHeld;
    mUpperHeld = mpMorf->getAnm();
    mpMorf->changeAnm(held);
}

// changeFaceBck, only while a face layer is on.
void DummyMidna::swapFace() {
    if (mFaceHeld == nullptr) {
        return;
    }
    J3DAnmTransform* held = mFaceHeld;
    mFaceHeld = mpMorf->getAnm();
    mpMorf->changeAnm(held);
}

// baseModelCallBack and modelCallBack, without demo positions and hair aiming.
void DummyMidna::jointCallBack(u16 jnt) {
    if (jnt == mFaceJoint) {
        swapFace();
    }
    if (jnt == MD_JNT_WORLD_ROOT_e || jnt == MD_JNT_HAND_R_e) {
        swapUpper();
    } else if (jnt == MD_JNT_MOUTH_e) {
        swapFace();
    } else if (jnt == MD_JNT_HEAD_e || jnt == MD_JNT_BACKBONE1_e || jnt == MD_JNT_BACKBONE2_e) {
        mDoMtx_stack_c::copy(J3DSys::mCurrentMtx);
        if (jnt == MD_JNT_HEAD_e) {
            mDoMtx_stack_c::YrotM(static_cast<s16>(-mNeckY));
            mDoMtx_stack_c::ZrotM(mNeckX);
        } else {
            mDoMtx_stack_c::ZrotM(mBackboneZ);
        }
        mpMorf->getModel()->setAnmMtx(jnt, mDoMtx_stack_c::get());
        cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    }
}

void DummyMidna::update(const RemoteMidnaPose& pose, J3DModel* wolf, float frameAlpha,
                        bool shown) {
    const bool apart = pose.mode == kMidnaApart;
    if (!mReady || (wolf == nullptr && !apart) || pose.mode == kMidnaNone) {
        deactivate();
        return;
    }
    const bool fresh = !mActive || mFresh;  // no morph from a pose of before
    mActive = true;
    mFresh = false;
    mMode = pose.mode;
    mNoShadow = (pose.flags & kMidnaNoShadow) != 0;
    mSilhouette = apart && (pose.flags & kMidnaSilhouette) != 0;
    if (shown && (mMode == kMidnaDrawn || apart)) {
        mShownTicks++;
    }

    const bool bodyChanged = bindBody(pose.bodyBck, fresh);
    if (mBodyBck == nullptr) {
        mActive = false;
        return;
    }
    J3DAnmTransform* upper =
        bindLayer(mUpperHeap, pose.upperBck, false, mUpperId, mUpperBck, mRefusedUpper);
    J3DAnmTransform* face =
        bindLayer(mFaceHeap, pose.faceBck, true, mFaceId, mFaceBck, mRefusedFace);
    const uint16_t upperShown = upper != nullptr ? mUpperId : 0;
    const uint16_t faceShown = face != nullptr ? mFaceId : 0;
    // A layer that comes or goes eases in, like a new upper clip does in setAnm.
    if (!fresh && !bodyChanged && (upperShown != mShownUpperId || faceShown != mShownFaceId)) {
        mpMorf->setMorf(kMorfFrames);
    }
    mShownUpperId = upperShown;
    mShownFaceId = faceShown;

    mpMorf->play(0, 0);  // steps the morph; the frame is the sender's
    mpMorf->setFrameF(normalizeFrame(
        mBodyBck, blendRemoteFrame(mBodyBck, pose.bodyFrame, pose.bodyFrameNext, frameAlpha)));
    if (bodyChanged || fresh) {
        bindSound();
    }
    float animFrame = mpMorf->getFrame();
    if (upper != nullptr) {
        animFrame = normalizeFrame(
            upper, blendRemoteFrame(upper, pose.upperFrame, pose.upperFrameNext, frameAlpha));
        upper->setFrame(animFrame);
    }
    if (face != nullptr) {
        animFrame = normalizeFrame(
            face, blendRemoteFrame(face, pose.faceFrame, pose.faceFrameNext, frameAlpha));
        face->setFrame(animFrame);
    }
    mUpperHeld = upper != nullptr ? upper : mBodyBck;  // no upper layer: the swaps change nothing
    mFaceHeld = face;
    mFaceJoint = (pose.flags & kMidnaFaceFromChin) ? MD_JNT_CHIN_e : MD_JNT_HEAD_e;

    bindTexture(true, pose.btp);
    bindTexture(false, pose.btk);
    updateFaceTextures(animFrame);
    // The sender's hair only leaves l_hairScale while it aims at something, which is not sent.
    mHairCB.mScale = (pose.flags & kMidnaNoHairScale) ? nullptr : kHairScale;
    mNeckX = pose.neckX;
    mNeckY = pose.neckY;
    mBackboneZ = pose.backboneZ;
    mEyeFlags = pose.eyeFlags;
    for (int i = 0; i < 2; i++) {
        if (mEyeAnm[i] != nullptr) {
            mEyeAnm[i]->mNowX = pose.eyeOffset[i * 2];
            mEyeAnm[i]->mNowY = pose.eyeOffset[i * 2 + 1];
        }
    }

    if (apart) {
        // setMatrix's WOLF_NO_POS branch: where the sender's Midna stands
        mPos.set(pose.worldPos[0], pose.worldPos[1], pose.worldPos[2]);
        mDoMtx_stack_c::transS(mPos);
        mDoMtx_stack_c::ZXYrotM(pose.worldAngle[0], pose.worldAngle[1], pose.worldAngle[2]);
        mpMorf->getModel()->setBaseTRMtx(mDoMtx_stack_c::get());
        mBackDist = 0.0f;
        mApartTicks++;
        mpMorf->modelCalc();
        updateParts(pose);
        updateSound(pose, shown);
        return;
    }

    // setMatrix, the branch for her place on the wolf's back (daAlink_c::getWolfMidnaMatrix).
    mDoMtx_stack_c::copy(wolf->getAnmMtx(WL_JNT_MD_e));
    mDoMtx_stack_c::ZXYrotM(-0x4000, -0x4000, 0);
    mDoMtx_stack_c::transM(-1.6f, -1.56f, -6.6f);
    if (mBodyId == dRes_ID_ALANM_BCK_MD_WLSWIMDIE_e && mMode == kMidnaDrawn) {
        mDoMtx_stack_c::multVecZero(&mPos);
    } else {
        const Vec offset = {0.0f, daMidna_hio_c0::m.y_pos, daMidna_hio_c0::m.z_pos};
        mDoMtx_stack_c::multVec(&offset, &mPos);
    }
    mDoMtx_stack_c::get()[0][3] = mPos.x;
    mDoMtx_stack_c::get()[1][3] = mPos.y;
    mDoMtx_stack_c::get()[2][3] = mPos.z;
    mpMorf->getModel()->setBaseTRMtx(mDoMtx_stack_c::get());
    cXyz back;
    mDoMtx_multVecZero(wolf->getAnmMtx(WL_JNT_MD_e), &back);
    mBackDist = mPos.abs(back);

    mpMorf->modelCalc();
    updateParts(pose);
    updateSound(pose, shown);
}

// setBodyPartMatrix for the models drawn with md.bmd.
void DummyMidna::updateParts(const RemoteMidnaPose& pose) {
    J3DModel* md = mpMorf->getModel();
    mDoMtx_multVecZero(md->getAnmMtx(MD_JNT_HEAD_e), &mEyePos);
    mpHands->setBaseTRMtx(md->getBaseTRMtx());
    mpHands->calc();
    mpHands->setAnmMtx(1, md->getAnmMtx(MD_JNT_HAND_L_e));
    mpHands->setAnmMtx(2, md->getAnmMtx(MD_JNT_HAND_R_e));
    mpMask->setBaseTRMtx(md->getAnmMtx(MD_JNT_HEAD_e));
    mpMask->calc();

    mDoMtx_stack_c::copy(md->getAnmMtx(MD_JNT_HAIR_5_e));
    mDoMtx_stack_c::transM(6.5f, 0.0f, 0.0f);
    mHairAimApplied = false;
    if (!(pose.flags & kMidnaHairFromBck)) {
        cXyz root, tip;
        mDoMtx_multVecZero(md->getAnmMtx(MD_JNT_HAIR_1_e), &root);
        mDoMtx_multVecZero(md->getAnmMtx(MD_JNT_HAIR_5_e), &tip);
        tip -= root;
        mDoMtx_stack_c::XYZrotM(0, pose.hairTipY,
                                static_cast<s16>(pose.hairTipZ - tip.atan2sY_XZ()));
    } else if (pose.hairAimValid) {
        // The lock target branch
        mDoMtx_stack_c::XrotM(pose.hairAim);
        mHairAimApplied = true;
        mHairAimAngle = pose.hairAim;
    }
    mpHairHand->setBaseTRMtx(mDoMtx_stack_c::get());
    mpHairHand->calc();
    if (pose.hairHand != mHairHand) {
        for (u16 i = 0; i < 3; i++) {
            showShape(mpHairHand, i, i == pose.hairHand);
        }
        mHairHand = pose.hairHand;
    }
    chaseHairColours(pose.hairHand != 0);
    setHandShapes(pose.leftHand, pose.rightHand);
    setTired((pose.flags & kMidnaTired) != 0);
}

// setLeftHandShape / setRightHandShape
void DummyMidna::setHandShapes(uint8_t left, uint8_t right) {
    const u16 count = (std::min)(materialCount(mpHands), static_cast<u16>(4));
    if (left >= count) {
        left = 0xFE;
    }
    if (right >= count) {
        right = 0xFE;
    }
    if (left == mLeftHand && right == mRightHand) {
        return;
    }
    J3DModel* md = mpMorf->getModel();
    showShape(md, 6, left == 0xFE);
    showShape(md, 7, right == 0xFE);
    for (u16 i = 0; i < count; i++) {
        showShape(mpHands, i, i == left || i == right);
    }
    mLeftHand = left;
    mRightHand = right;
}

// Link binds the tired colours on the local wolf's Midna models every tick (daAlink_c::execute).
void DummyMidna::setTired(bool tired) {
    if (tired == mTired) {
        return;
    }
    J3DModel* owners[4] = {mpMorf->getModel(), mpMask, mpHands, mpHairHand};
    for (int i = 0; i < 4; i++) {
        if (mTiredBrk[i] == nullptr) {
            continue;
        }
        J3DModelData* data = owners[i]->getModelData();
        if (tired) {
            data->entryTevRegAnimator(mTiredBrk[i]);
        } else {
            data->removeTevRegAnimator(mTiredBrk[i]);
        }
    }
    mTired = tired;
}

// daMidna_c::create.
void DummyMidna::initHairColours() {
    const bool dark = dKy_darkworld_check() != 0;
    mHairColor.r = kNormalColor.r;
    mHairColor.g = kNormalColor.g;
    mHairColor.b = kNormalColor.b;
    mHairColor.a = kNormalColor.a;
    static_cast<GXColor&>(mHairK1) = dark ? kNormalKColor : kLightNormalKColor;
    static_cast<GXColor&>(mHairK2) = dark ? kNormalKColor2 : kLightNormalKColor2;
}

// setBodyPartMatrix
void DummyMidna::chaseHairColours(bool big) {
    const bool dark = dKy_darkworld_check() != 0;
    const GXColorS10& color = big ? (dark ? kBigColor : kLightBigColor) : kNormalColor;
    const GXColor& kColor1 = big ? kBigKColor : (dark ? kNormalKColor : kLightNormalKColor);
    const GXColor& kColor2 =
        big ? (dark ? kNormalKColor2 : kLightBigKColor2) : (dark ? kNormalKColor2 : kLightNormalKColor2);
    cLib_chaseS(&mHairColor.r, color.r, 10);
    cLib_chaseS(&mHairColor.g, color.g, 10);
    cLib_chaseS(&mHairColor.b, color.b, 10);
    cLib_chaseUC(&mHairK1.r, kColor1.r, 10);
    cLib_chaseUC(&mHairK1.g, kColor1.g, 10);
    cLib_chaseUC(&mHairK1.b, kColor1.b, 10);
    cLib_chaseUC(&mHairK2.r, kColor2.r, 10);
    cLib_chaseUC(&mHairK2.g, kColor2.g, 10);
    cLib_chaseUC(&mHairK2.b, kColor2.b, 10);
}

// daMidna_c::draw's md.bmd branch.
void DummyMidna::draw(const dKy_tevstr_c& owner, bool ownerTinted) {
    if (!mActive || (mMode != kMidnaDrawn && mMode != kMidnaApart)) {
        return;
    }
    J3DModel* md = mpMorf->getModel();
    dComIfGd_setListDark();
    mTevStr.room_no = owner.room_no;
    mTevStr.YukaCol = owner.YukaCol;
    g_env_light.settingTevStruct(3, &mPos, &mTevStr);
    // Off the back as her shadow: the gokou/inv branch's dark figure, here from md.bmd.
    if (mSilhouette) {
        mTevStr.TevColor.r = mTevStr.TevColor.g = mTevStr.TevColor.b = -220;
    } else if (mBodyId == dRes_ID_ALANM_BCK_MD_RETURN_e && mpMorf->getEndFrame() > 0.0f) {
        // MD_RETURN fades her into the wolf's shadow.
        const s16 fade = static_cast<s16>(mpMorf->getFrame() / mpMorf->getEndFrame() * -32.0f);
        mTevStr.TevColor.r = mTevStr.TevColor.g = mTevStr.TevColor.b = fade;
    } else if (ownerTinted) {
        mTevStr.TevColor = owner.TevColor;
    }
    g_env_light.setLightTevColorType_MAJI(md, &mTevStr);
    J3DMaterial* hair = md->getModelData()->getMaterialNodePointer(4);
    hair->setTevColor(1, &mHairColor);
    hair->setTevKColor(1, &mHairK1);
    mDoExt_modelEntryDL(md);
    g_env_light.setLightTevColorType_MAJI(mpHands, &mTevStr);
    mDoExt_modelEntryDL(mpHands);
    for (u16 i = 0; i < 3; i++) {
        J3DMaterial* material = mpHairHand->getModelData()->getMaterialNodePointer(i);
        material->setTevColor(1, &mHairColor);
        material->setTevKColor(1, &mHairK1);
        material->setTevKColor(2, &mHairK2);
    }
    g_env_light.setLightTevColorType_MAJI(mpHairHand, &mTevStr);
    mDoExt_modelEntryDL(mpHairHand);
    g_env_light.setLightTevColorType_MAJI(mpMask, &mTevStr);
    mDoExt_modelEntryDL(mpMask);
    mTevStr.TevColor.r = mTevStr.TevColor.g = mTevStr.TevColor.b = 0;
    dComIfGd_setList();
}

// The wolf's real shadow carries her whenever she rides, drawn or not (daAlink_c::draw).
void DummyMidna::addRealShadow(u32 shadowId) {
    if (!mActive || mNoShadow || shadowId == 0) {
        return;
    }
    dComIfGd_addRealShadow(shadowId, mpMorf->getModel());
    dComIfGd_addRealShadow(shadowId, mpMask);
    dComIfGd_addRealShadow(shadowId, mpHairHand);
}

}  // namespace twili

