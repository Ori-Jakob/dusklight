#include "actors/DummyHorse.hpp"

#include "core/GameAccess.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_resorce.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/Horse.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twili {
namespace {

constexpr const char* kHorseArcPath = "/res/Object/Horse.arc";
// Horse.arc decompressed is about 0x59000; the heap is trimmed to what createHeap used.
constexpr u32 kDummyHorseHeapSize = 0x60000;
constexpr u32 kDummyHorseHeapFlags = 0x80000000 | 0x20000000;
constexpr u16 kJointNum = 38;
constexpr u16 kWaitAnm = dRes_ID_HORSE_BCK_HS_WAIT_01_e;
// Clip changes blend in like the dummy's Link packs: the sender's morph lengths are not sent.
constexpr f32 kMorfFrames = 3.0f;
constexpr f32 kBlinkChance = 0.012f;  // daHorse_c::animePlay
constexpr f32 kAudibleDistanceSq = 3500.0f * 3500.0f;
// The owner's dummy pushes a pose every tick; this many of our ticks without one and it hides.
constexpr uint32_t kMaxPushGapTicks = 2;
constexpr u16 kSaddleJoint = 21;
constexpr u16 kHeadJoint = 15;
constexpr u16 kLeftStirrupJoint = 0x17;
constexpr u16 kRightStirrupJoint = 0x19;
constexpr int kReinSideCount = 24;      // setReinPosNormalSubstance
constexpr int kReinHandSideCount = 20;  // setReinPosHandSubstance
constexpr int kReinOneHandSideCount = 24;

u32 archiveNodeType(JKRArchive* archive, u16 fileIndex) {
    JKRArchive::SDIDirEntry* node = archive->mNodes;
    for (s32 i = 0; i < archive->countDirectory(); i++, node++) {
        const u32 first = node->first_file_index;
        if (fileIndex >= first && fileIndex < first + node->num_entries) {
            return node->type;
        }
    }
    return 'BMDR';
}

// dRes_info_c's BCK path on a private file: the clip with its BAS. Each index is loaded once
// (J3DAnmLoader swaps the key data in place).
mDoExt_transAnmBas* loadBckWithBas(JKRArchive* archive, u16 index) {
    if (!archive->isFileEntry(index)) {
        return nullptr;
    }
    void* res = archive->getIdxResource(index);
    if (res == nullptr) {
        return nullptr;
    }
    struct BckHeader {
        u8 unk[0x1C];
        BE(u32) basOffset;
    };
    const u32 basOffset = static_cast<BckHeader*>(res)->basOffset;
    void* bas = basOffset != 0xFFFFFFFF ? static_cast<u8*>(res) + basOffset : nullptr;
    auto* anm = JKR_NEW mDoExt_transAnmBas(bas);
    if (anm == nullptr) {
        return nullptr;
    }
    J3DAnmLoaderDataBase::setResource(anm, res);
    return anm;
}

int daDummyHorse_jointCallBack(J3DJoint* joint, int timing) {
    if (timing == 0) {
        reinterpret_cast<daHorse_c*>(j3dSys.getModel()->getUserArea())
            ->modelCallBack(joint->getJntNo());
    }
    return 1;
}

int daDummyHorse_createHeap(fopAc_ac_c* actor) {
    return static_cast<daDummyHorse_c*>(actor)->createHeap();
}

bool sameStage(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strncmp(a, b, 8) == 0;
}

// `frame` moved toward `next` by `alpha`, a looping clip's wrap taken the short way.
f32 blendFrame(J3DAnmTransform* bck, f32 frame, f32 next, f32 alpha) {
    const f32 max = bck->getFrameMax();
    if (alpha <= 0.0f || max <= 0.0f) {
        return frame;
    }
    if (bck->getAttribute() == J3DFrameCtrl::EMode_LOOP && next < frame &&
        frame - next > max * 0.5f)
    {
        next += max;
    }
    f32 out = frame + (next - frame) * alpha;
    if (out >= max) {
        out = bck->getAttribute() == J3DFrameCtrl::EMode_LOOP ? std::fmod(out, max) : max;
    }
    return out;
}

}  // namespace

bool GetDummyHorseDebugInfo(fopAc_ac_c* actor, DummyHorseDebugInfo& out) {
    if (actor == nullptr || g_procDummyHorse < 0 || fopAcM_GetName(actor) != g_procDummyHorse) {
        return false;
    }
    static_cast<daDummyHorse_c*>(actor)->getDebugInfo(out);
    return true;
}

daDummyHorse_c* FindDummyHorse(uint32_t clientId) {
    if (!Session::active()) {
        return nullptr;
    }
    fopAc_ac_c* actor = Session::instance().horseActorForClient(clientId);
    if (actor == nullptr || g_procDummyHorse < 0 || fopAcM_GetName(actor) != g_procDummyHorse) {
        return nullptr;
    }
    return static_cast<daDummyHorse_c*>(actor);
}

// A failed attempt's heap dies without unlinking what was mounted into it from the volume list.
int daDummyHorse_c::createHeap() {
    const int result = createHeapImpl();
    if (!result && mpArchive != nullptr) {
        mpArchive->unmount();
        mpArchive = nullptr;
    }
    return result;
}

int daDummyHorse_c::createHeapImpl() {
    mpArchive = nullptr;
    std::fill(std::begin(mBck), std::end(mBck), nullptr);
    mRecolor = HorseRecolor{};
    mpArchive = JKRArchive::mount(kHorseArcPath, JKRArchive::MOUNT_MEM, mDoExt_getCurrentHeap(),
        JKRArchive::MOUNT_DIRECTION_HEAD);
    if (mpArchive == nullptr) {
        TwiliLog.warn("[horse {}] createHeap: {} mount failed", mClientId, kHorseArcPath);
        return 0;
    }
    const u16 bmd = dRes_INDEX_HORSE_BMD_HS_e;
    void* raw = mpArchive->isFileEntry(bmd) ? mpArchive->getIdxResource(bmd) : nullptr;
    // Loaded once: J3DModelLoader swaps the raw BMD in place.
    m_modelData = raw != nullptr ?
                      dRes_info_c::loaderBasicBmd(archiveNodeType(mpArchive, bmd), raw) :
                      nullptr;
    if (m_modelData == nullptr || m_modelData->getJointNum() != kJointNum) {
        TwiliLog.warn("[horse {}] createHeap: hs.bmd missing or not the horse", mClientId);
        return 0;
    }
    // daHorse_c::createHeap's flags.
    m_model = mDoExt_J3DModel__create(m_modelData, 0x80000, 0x11020084);
    if (m_model == nullptr) {
        return 0;
    }
    // The mane's palette, before the heap is trimmed.
    if (!bindHorseRecolor(mRecolor, m_modelData, mDoExt_getCurrentHeap(), mClientId)) {
        TwiliLog.warn("[horse {}] createHeap: mane recolour not bound", mClientId);
    }
    J3DTransformInfo* transInfo = JKR_NEW_ARRAY(J3DTransformInfo, kJointNum);
    Quaternion* quat = JKR_NEW_ARRAY(Quaternion, kJointNum);
    if (transInfo == nullptr || quat == nullptr) {
        return 0;
    }
    m_oldFrame = JKR_NEW mDoExt_MtxCalcOldFrame(transInfo, quat);
    if (m_oldFrame == nullptr) {
        return 0;
    }
    m_mtxcalc = JKR_NEW mDoExt_MtxCalcAnmBlendTblOld(m_oldFrame, 3, m_anmRatio);
    if (m_mtxcalc == nullptr) {
        return 0;
    }
    const u16 btp = dRes_INDEX_HORSE_BTP_HS_EYE_e;
    J3DAnmBase* eyes = mpArchive->isFileEntry(btp) ?
                           J3DAnmLoaderDataBase::load(mpArchive->getIdxResource(btp)) :
                           nullptr;
    if (eyes == nullptr || eyes->getKind() != 2 /* J3DAnmTexPattern */ ||
        !m_btp.init(m_modelData, static_cast<J3DAnmTexPattern*>(eyes), FALSE,
            J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1))
    {
        return 0;
    }
    const u16 bti = dRes_INDEX_HORSE_BTI_TAZUNA_e;
    auto* reinTex = mpArchive->isFileEntry(bti) ?
                        static_cast<ResTIMG*>(mpArchive->getIdxResource(bti)) :
                        nullptr;
    if (reinTex == nullptr || !m_reinLine.init(1, 75, reinTex, 0)) {
        return 0;
    }
    m_rein[0].field_0x8[1] = 35;
    m_rein[1].field_0x8[1] = 35;
    m_rein[2].field_0x8[1] = 5;
    for (daHorseRein_c& rein : m_rein) {
        for (int k = 0; k < 2; k++) {
            rein.field_0x0[k] = JKR_NEW_ARRAY(cXyz, rein.field_0x8[1]);
            if (rein.field_0x0[k] == nullptr) {
                return 0;
            }
            std::fill(rein.field_0x0[k], rein.field_0x0[k] + rein.field_0x8[1], cXyz::Zero);
        }
    }
    for (u16 i = kHorseFirstAnm; i <= kHorseLastAnm; i++) {
        mBck[i] = loadBckWithBas(mpArchive, i);
        if (mBck[i] == nullptr) {
            TwiliLog.warn("[horse {}] createHeap: clip {} missing", mClientId, i);
            return 0;
        }
    }
    JKRHeap* heap = mDoExt_getCurrentHeap();
    mHeapUsed = heap->getHeapSize() - heap->getFreeSize();
    TwiliLog.info("[horse {}] createHeap OK, heap used 0x{:X} of 0x{:X}", mClientId, mHeapUsed,
        heap->getHeapSize());
    return 1;
}

cPhs_Step daDummyHorse_c::create() {
    // daHorse_c's member constructors only set fields; nothing registers a horse here.
    fopAcM_ct(this, daDummyHorse_c);
    mClientId = static_cast<uint32_t>(fopAcM_GetParam(this));
    mBlinkRng.seed(mClientId + 1);
    fopAcM_setStageLayer(this);
    if (!fopAcM_entrySolidHeap(
            this, daDummyHorse_createHeap, kDummyHorseHeapSize | kDummyHorseHeapFlags))
    {
        TwiliLog.warn("[horse {}] create: entrySolidHeap failed", mClientId);
        return cPhs_ERROR_e;
    }
    // daHorse_c::create's static set-up minus everything that talks to the game: no
    // dComIfGp_setHorseActor, no paths, no collision, no acch.
    m_modelData->getJointNodePointer(0)->setMtxCalc(m_mtxcalc);
    m_model->setUserArea(reinterpret_cast<uintptr_t>(this));
    for (u16 i = 0; i < kJointNum; i++) {
        m_modelData->getJointNodePointer(i)->setCallBack(daDummyHorse_jointCallBack);
    }
    // Never PROC_TOOL_DEMO_e: modelCallBack would pin the root to current.pos.
    m_procID = PROC_WAIT_e;
    for (int i = 0; i < 3; i++) {
        m_anmRatio[i].setAnmTransform(nullptr);
        m_anmRatio[i].setRatio(0.0f);
        m_anmIdx[i] = 0xFFFF;
    }
    m_stateFlg0 = 0;
    m_resetStateFlg0 = 0;
    m_endResetStateFlg0 = 0;  // ERFLG0_UNK_40 off: the tail procedural always runs
    attention_info.flags = 0;
    shape_angle.z = 0;
    current.angle.z = 0;
    this->model = m_model;
    fopAcM_SetMtx(this, m_model->getBaseTRMtx());
    // Unridden: a rider would also start his harness sounds on OUR Link (Z2RideSoundStarter).
    m_sound.init(&current.pos, &eyePos, 6, 1);
    m_sound.setLinkRiding(false);
    m_shadowID = 0;
    field_0x1204 = 0;
    TwiliLog.info("[horse {}] create complete", mClientId);
    return cPhs_COMPLEATE_e;
}

const Client* daDummyHorse_c::ownerClient() const {
    if (!Session::active()) {
        return nullptr;
    }
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(mClientId);
    return it != clients.end() ? &it->second : nullptr;
}

bool daDummyHorse_c::parkedWanted(const Client& c) const {
    const char* myStage = dComIfGp_getStartStageName();
    return c.online && c.isSaveLoaded && c.horsePlace.valid &&
           sameStage(c.horsePlace.stage, myStage) && !sameStage(c.stageName, myStage);
}

int daDummyHorse_c::execute() {
    const Client* c = ownerClient();
    if (c == nullptr) {
        // Its client left or we disconnected meanwhile; nothing else would remove it.
        fopAcM_delete(this);
        return TRUE;
    }
    attention_info.flags = 0;
    mExecTicks++;
    if (parkedWanted(*c)) {
        // No rider whose cutscene could hide it: only ours does.
        mHidden = hideRemotePlayersForCutscene();
        poseParked(*c);
    } else {
        mHidden = hideRemotePlayersForCutscene() || hideRemoteClientForCutscene(*c);
        if (mParked) {
            // Its owner came back: its stream poses it from now on.
            mParked = false;
            mPosed = false;
        }
        if (mExecTicks - mLastPushTick > kMaxPushGapTicks) {
            mPosed = false;
            mRidden = false;
        }
    }
    updateHorseRecolor(mRecolor, c->colorR, c->colorG, c->colorB);
    updateSound(isShown());
    return TRUE;
}

bool daDummyHorse_c::bindPack(int pack, uint16_t anm, float frame, float frameNext, float alpha) {
    J3DAnmTransform* bck = anm != 0 ? mBck[anm] : nullptr;
    const bool changed = anm != mBoundAnm[pack];
    mBoundAnm[pack] = anm;
    m_anmIdx[pack] = anm != 0 ? anm : 0xFFFF;
    m_anmRatio[pack].setAnmTransform(bck);
    if (bck != nullptr) {
        bck->setFrame(blendFrame(bck, frame, frameNext, alpha));
    }
    return changed;
}

void daDummyHorse_c::applyRemotePose(const RemoteHorsePose& p, float alpha, bool snapped) {
    mLastPushTick = mExecTicks;
    if (mParked) {
        mParked = false;
        mPosed = false;
    }
    if (!p.present()) {
        mPosed = false;
        mRidden = false;
        mDemoHeld = false;
        return;
    }
    if (p.flags & kHorseDemo) {
        // A stage demo clip cannot be posed here: the last pose holds, hidden.
        mDemoHeld = true;
        mRidden = false;
        return;
    }
    const bool jump = snapped || !mPosed || mDemoHeld || p.epoch != mEpoch;
    mDemoHeld = false;
    mEpoch = p.epoch;
    mFlags = p.flags;
    const cXyz prev = current.pos;
    current.pos.set(p.pos[0], p.pos[1], p.pos[2]);
    old.pos = jump ? current.pos : prev;
    shape_angle.set(p.angle[0], p.angle[1], p.angle[2]);
    current.angle.y = shape_angle.y;
    // The procedural joints exactly as the sender's modelCallBack had them.
    field_0x16f0 = p.neckYaw;
    field_0x16fa = p.lean;
    for (int i = 0; i < 3; i++) {
        field_0x16d4[i] = p.tail[i];
    }
    for (int f = 0; f < 4; f++) {
        for (int j = 0; j < 4; j++) {
            m_footData[f].field_0x4[j] = p.foot[f][j];
        }
    }
    uint16_t anm0 = p.anm[0];
    if (anm0 == 0) {
        // The sender's pack 0 always holds a clip: this one was junk.
        mRefused++;
        anm0 = kWaitAnm;
    }
    bool bodyChanged = bindPack(0, anm0, p.frame[0], p.frameNext[0], alpha);
    bodyChanged |= bindPack(1, p.anm[1], p.frame[1], p.frameNext[1], alpha);
    const bool neckChanged = bindPack(2, p.anm[2], p.frame[2], p.frameNext[2], alpha);
    m_anmRatio[0].setRatio(p.anm[1] != 0 ? p.ratio[0] : 1.0f);
    m_anmRatio[1].setRatio(p.anm[1] != 0 ? p.ratio[1] : 0.0f);
    if (p.anm[2] == 0) {
        m_anmRatio[2].setRatio(0.0f);
    }
    if (jump) {
        m_oldFrame->initOldFrameMorf(0.0f, 0, kJointNum);
    } else if (bodyChanged) {
        m_oldFrame->initOldFrameMorf(kMorfFrames, 0, kJointNum);
    } else if (neckChanged) {
        // setNeckAnimeMorf: the neck alone, unless a whole-body morph is still running.
        if (m_oldFrame->getOldFrameRate() > 0.1f && m_oldFrame->getOldFrameStartJoint() == 0) {
            m_oldFrame->initOldFrameMorf(kMorfFrames, 0, kJointNum);
        } else {
            m_oldFrame->initOldFrameMorf(kMorfFrames, 11, 21);
        }
    }
    m_resetStateFlg0 = (p.flags & kHorseReinReset) || jump ? RFLG0_UNK_1 : 0;
    // The bags, on this puppet's own model data.
    if (p.flags & kHorseBagHidden) {
        offBagMaterial();
    } else {
        onBagMaterial();
    }
    finishPose(jump);
    mRidden = p.rider.active && (p.flags & kHorseRidden);
    if (!mRidden) {
        updateReinsNormal(m_resetStateFlg0 != 0);
    }
    mPosed = true;
}

void daDummyHorse_c::poseParked(const Client& c) {
    const HorsePlace& place = c.horsePlace;
    const cXyz pos(place.pos[0], place.pos[1], place.pos[2]);
    const bool jump =
        !mPosed || !mParked || pos.abs2(current.pos) > 1.0f || place.angleY != shape_angle.y;
    mParked = true;
    mDemoHeld = false;
    mRidden = false;
    mFlags = kHorsePresent;
    current.pos = pos;
    old.pos = pos;
    shape_angle.set(0, place.angleY, 0);
    current.angle.y = shape_angle.y;
    field_0x16f0 = 0;
    field_0x16fa = 0;
    for (int i = 0; i < 3; i++) {
        field_0x16d4[i] = 0;
    }
    for (daHorseFootData_c& foot : m_footData) {
        std::fill(std::begin(foot.field_0x4), std::end(foot.field_0x4), 0);
    }
    // Her idle clip, played here: nothing streams a horse whose owner is elsewhere.
    J3DAnmTransform* wait = mBck[kWaitAnm];
    mParkedFrame = jump ? 0.0f : mParkedFrame + 1.0f;
    if (mParkedFrame >= wait->getFrameMax()) {
        mParkedFrame = 0.0f;
    }
    bindPack(0, kWaitAnm, mParkedFrame, mParkedFrame, 0.0f);
    bindPack(1, 0, 0.0f, 0.0f, 0.0f);
    bindPack(2, 0, 0.0f, 0.0f, 0.0f);
    m_anmRatio[0].setRatio(1.0f);
    m_anmRatio[1].setRatio(0.0f);
    m_anmRatio[2].setRatio(0.0f);
    if (jump) {
        m_oldFrame->initOldFrameMorf(0.0f, 0, kJointNum);
    }
    m_resetStateFlg0 = jump ? RFLG0_UNK_1 : 0;
    onBagMaterial();
    finishPose(jump);
    updateReinsNormal(jump);
    mPosed = true;
}

// What daHorse_c::execute does after its calc, for the parts a puppet shows.
void daDummyHorse_c::finishPose(bool jump) {
    initHorseMtx();  // one calc per tick: the morph counts ticks
    setBodyPart();   // eyePos from the head
    field_0x1710 = jump ? shape_angle.y : field_0x170e;
    field_0x170e = shape_angle.y;
    cXyz neck;
    cXyz head;
    mDoMtx_multVecZero(m_model->getAnmMtx(0xB), &neck);
    mDoMtx_multVecZero(m_model->getAnmMtx(0x14), &head);
    head -= neck;
    field_0x1712 =
        std::clamp<s16>(static_cast<s16>(head.atan2sX_Z() - field_0x170e), -0x2000, 0x2000);
    attention_info.position.set(current.pos.x, current.pos.y + 200.0f, current.pos.z);
    attention_info.flags = 0;
    updateGround();
    if (mBlinkFrame != 0) {
        if (++mBlinkFrame > m_btp.getBtpAnm()->getFrameMax()) {
            mBlinkFrame = 0;
        }
    } else if (std::uniform_real_distribution<float>(0.0f, 1.0f)(mBlinkRng) < kBlinkChance) {
        mBlinkFrame = 1;
    }
    if (jump && isShown()) {
        interp::requestPresentationSync();
    }
}

// The ground for the shadow, the light and the hoof sounds; none while its room is not loaded.
void daDummyHorse_c::updateGround() {
    cXyz start(current.pos.x, current.pos.y + 100.0f, current.pos.z);
    mGndChk.SetPos(&start);
    mGroundY = dComIfG_Bgsp().GroundCross(&mGndChk);
    if (mGroundY > -G_CM3D_F_INF) {
        tevStr.room_no = dComIfG_Bgsp().GetRoomId(mGndChk);
        tevStr.YukaCol = dComIfG_Bgsp().GetPolyColor(mGndChk);
        m_poly_sound = dKy_pol_sound_get(&mGndChk);
    } else {
        m_poly_sound = 0;
    }
    if (tevStr.room_no < 0) {
        tevStr.room_no = dComIfGp_roomControl_getStayNo();
    }
    m_reverb = dComIfGp_getReverb(tevStr.room_no);
}

// The hoofbeats and snorts keyed to its clips' BAS, played here; silent while hidden or far.
void daDummyHorse_c::updateSound(bool shown) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const bool audible =
        shown && player != nullptr && player->current.pos.abs2XZ(current.pos) <= kAudibleDistanceSq;
    if (!audible) {
        // The clip's BAS starts over once it is heard again (stopAnime would free handle pools).
        mSoundPack = -1;
        return;
    }
    // daHorse_c::setDoubleAnime: the BAS of the pack that dominates.
    const int pack =
        m_anmRatio[1].getAnmTransform() != nullptr && m_anmRatio[1].getRatio() >= 0.5f ? 1 : 0;
    const uint16_t anm = mBoundAnm[pack];
    mDoExt_transAnmBas* bck = anm != 0 ? mBck[anm] : nullptr;
    if (bck == nullptr) {
        return;
    }
    const f32 frame = bck->getFrame();
    if (pack != mSoundPack || anm != mSoundAnm) {
        mSoundPack = static_cast<int8_t>(pack);
        mSoundAnm = anm;
        mSoundFrame = frame;
        if (bck->getBas() != nullptr) {
            m_sound.initAnime(bck->getBas(), true, 0.0f, frame);
            mSoundAnims++;
        }
    }
    f32 rate = frame - mSoundFrame;
    if (rate < 0.0f && bck->getAttribute() == J3DFrameCtrl::EMode_LOOP) {
        rate += bck->getFrameMax();
    }
    rate = std::clamp(rate, 0.0f, 4.0f);
    mSoundFrame = frame;
    m_sound.framework(m_poly_sound, m_reverb);
    m_sound.updateAnime(frame, rate);
}

bool daDummyHorse_c::riderWorldPos(const RemoteHorseRider& rider, cXyz& out) {
    if (!mPosed || mParked || mDemoHeld || !rider.active) {
        return false;
    }
    const cXyz off(rider.off[0], rider.off[1], rider.off[2]);
    mDoMtx_multVec(getRootMtx(), &off, &out);
    return true;
}

void daDummyHorse_c::applyRider(daAlink_c& rider, const RemoteHorsePose& p) {
    if (!mPosed || mParked || mDemoHeld || !mRidden) {
        return;
    }
    const RemoteHorseRider& r = p.rider;
    // setAnmMtx records the joint for frame interpolation.
    if (r.stirrups & 1) {
        mDoMtx_stack_c::copy(rider.mpLinkModel->getAnmMtx(rider.field_0x30bc));
        mDoMtx_stack_c::transM(-2.0f, -11.0f, 1.5f);
        mDoMtx_stack_c::ZXYrotM(0, -0x8000, 0x4000);
        m_model->setAnmMtx(kLeftStirrupJoint, mDoMtx_stack_c::get());
    }
    if (r.stirrups & 2) {
        mDoMtx_stack_c::copy(rider.mpLinkModel->getAnmMtx(rider.field_0x30be));
        mDoMtx_stack_c::transM(-2.0f, 11.0f, 1.5f);
        mDoMtx_stack_c::ZrotM(-0x4000);
        m_model->setAnmMtx(kRightStirrupJoint, mDoMtx_stack_c::get());
    }
    if (r.stirrups & 3) {
        m_model->calcWeightEnvelopeMtx();
        // No exported call records the weight matrices (setHorseStirrup does it for our horse).
        for (u16 i = 0; i < m_modelData->getWEvlpMtxNum(); i++) {
            MtxP mtx = m_model->getWeightAnmMtx(i);
            interp::recordFinalMtx(mtx, mtx);
        }
    }
    const bool reset = m_resetStateFlg0 != 0;
    if (r.reinHand == 0) {
        updateReinsNormal(reset);
    } else if (r.reinHand > 0) {
        updateReinsHand(rider, r.reinHand, reset);
    }
}

// setReinPosNormalSubstance without the Zelda branch.
void daDummyHorse_c::updateReinsNormal(bool reset) {
    static const cXyz saddleLeft(29.0f, -2.0f, 30.0f);
    static const cXyz saddleRight(29.0f, 2.0f, 30.0f);
    setReinPosMoveInit(0);
    for (int n = 0; n < (reset ? 6 : 1); n++) {
        mDoMtx_multVec(
            m_model->getAnmMtx(kSaddleJoint), &saddleLeft, m_rein[0].field_0x0[0] + kReinSideCount);
        m_rein[0].setReinPosPart(kReinSideCount);
        mDoMtx_multVec(m_model->getAnmMtx(kSaddleJoint), &saddleRight,
            m_rein[1].field_0x0[0] + kReinSideCount);
        m_rein[1].setReinPosPart(kReinSideCount);
        *m_rein[2].field_0x0[0] = m_rein[0].field_0x0[0][kReinSideCount];
        m_rein[2].setReinPosPart(0);
    }
    copyReinsToLine();
}

// setReinPosHandSubstance with the dummy's hands and without the Zelda branch.
void daDummyHorse_c::updateReinsHand(daAlink_c& rider, int type, bool reset) {
    setReinPosMoveInit(type);
    int side = (-field_0x1712 * 5) / 0x2000;
    if (type != 3) {
        side *= 2;
    }
    const int left = (type & 1) ? side + kReinHandSideCount : side + kReinOneHandSideCount;
    const int right = (type & 2) ? kReinHandSideCount - side : kReinOneHandSideCount - side;
    MtxP leftHand = rider.getLeftHandMatrix();
    MtxP rightHand = rider.getRightHandMatrix();
    for (int n = 0; n < (reset ? 6 : 1); n++) {
        cXyz* a = &m_rein[0].field_0x0[0][left];
        cXyz* b = &m_rein[1].field_0x0[0][right];
        if (type == 2) {
            mDoMtx_multVec(rightHand, &daAlink_c::m_handRightInSidePos, a);
            mDoMtx_multVec(rightHand, &daAlink_c::m_handRightOutSidePos, b);
        } else if (type == 1) {
            mDoMtx_multVec(leftHand, &daAlink_c::m_handLeftOutSidePos, a);
            mDoMtx_multVec(leftHand, &daAlink_c::m_handLeftInSidePos, b);
        } else {
            mDoMtx_multVec(leftHand, &daAlink_c::m_handLeftOutSidePos, a);
            mDoMtx_multVec(rightHand, &daAlink_c::m_handRightOutSidePos, b);
        }
        m_rein[0].setReinPosPart(left);
        m_rein[1].setReinPosPart(right);
        cXyz* center = m_rein[2].field_0x0[0];
        mDoMtx_multVec(leftHand, &daAlink_c::m_handLeftInSidePos, center);
        if (type == 3) {
            mDoMtx_multVec(rightHand, &daAlink_c::m_handRightInSidePos, center + 4);
            m_rein[2].setReinPosPart(4);
        } else {
            m_rein[2].setReinPosPart(0);
        }
    }
    copyReinsToLine();
}

// daHorse_c::copyReinPos; the line interpolates itself when it is updated in draw.
void daDummyHorse_c::copyReinsToLine() {
    cXyz* pos = m_reinLine.getPos(0);
    field_0x1204 = m_rein[0].field_0x8[0];
    for (int i = 0; i < m_rein[0].field_0x8[0]; i++) {
        *pos++ = m_rein[0].field_0x0[0][i];
    }
    if (m_rein[2].field_0x8[0] > 1) {
        field_0x1204 += m_rein[2].field_0x8[0];
        for (int i = 0; i < m_rein[2].field_0x8[0]; i++) {
            *pos++ = m_rein[2].field_0x0[0][i];
        }
    }
    field_0x1204 += m_rein[1].field_0x8[0];
    for (int i = m_rein[1].field_0x8[0] - 1; i >= 0; i--) {
        *pos++ = m_rein[1].field_0x0[0][i];
    }
}

bool daDummyHorse_c::isShown() const {
    // The local cutscene again, so it applies from its first frame.
    return mPosed && !mDemoHeld && !mHidden && !hideRemotePlayersForCutscene();
}

int daDummyHorse_c::draw() {
    const Client* c = ownerClient();
    // An early return: fopAc_Draw clears fopAcStts_NODRAW_e every draw.
    if (c == nullptr || !isShown()) {
        return TRUE;
    }
    g_env_light.settingTevStruct(0, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(m_model, &tevStr);
    // After MAJI, which rewrites C0.
    applyHorseColor(mRecolor, m_modelData, tevStr, c->colorR, c->colorG, c->colorB);
    // Unlike daHorse_c::draw, never hide the mane for our own first-person camera.
    m_btp.entry(m_modelData, static_cast<f32>(mBlinkFrame));
    mDoExt_modelEntryDL(m_model);
    mDraws++;
    if (mGroundY > -G_CM3D_F_INF) {
        cXyz shadowPos(current.pos.x, current.pos.y + 100.0f, current.pos.z);
        m_shadowID =
            dComIfGd_setShadow(m_shadowID, 0, m_model, &shadowPos, 1000.0f, 0.0f, current.pos.y,
                mGroundY, mGndChk, &tevStr, 0, 1.0f, dDlst_shadowControl_c::getSimpleTex());
    } else {
        m_shadowID = 0;
    }
    if (!(mFlags & kHorseReinsHidden) && field_0x1204 > 1) {
        static GXColor reinLineColor = {0x00, 0x00, 0x00, 0xFF};
        m_reinLine.update(field_0x1204, 1.5f, reinLineColor, 0, &tevStr);
        dComIfGd_set3DlineMat(&m_reinLine);
    }
    return TRUE;
}

void daDummyHorse_c::destroy() {
    // Never ~daHorse_c: it would drop the resident "Horse" archive our own Epona uses.
    m_sound.deleteObject();
    if (mpArchive != nullptr) {
        mpArchive->unmount();
        mpArchive = nullptr;
    }
    TwiliLog.info("[horse {}] destroyed", mClientId);
}

void daDummyHorse_c::getDebugInfo(DummyHorseDebugInfo& out) const {
    out = DummyHorseDebugInfo{};
    out.clientId = mClientId;
    out.parked = mParked;
    out.posed = mPosed;
    out.shown = isShown();
    out.ridden = isRidden();
    out.demoHeld = mDemoHeld;
    for (int i = 0; i < 3; i++) {
        out.anm[i] = mBoundAnm[i];
    }
    out.pos[0] = current.pos.x;
    out.pos[1] = current.pos.y;
    out.pos[2] = current.pos.z;
    if (m_model != nullptr) {
        cXyz head;
        cXyz saddle;
        mDoMtx_multVecZero(m_model->getAnmMtx(kHeadJoint), &head);
        mDoMtx_multVecZero(m_model->getAnmMtx(kSaddleJoint), &saddle);
        out.headPos[0] = head.x;
        out.headPos[1] = head.y;
        out.headPos[2] = head.z;
        out.saddlePos[0] = saddle.x;
        out.saddlePos[1] = saddle.y;
        out.saddlePos[2] = saddle.z;
    }
    out.attentionFlags = attention_info.flags;
    out.refused = mRefused;
    out.heapUsed = mHeapUsed;
    out.draws = mDraws;
    out.reinPoints = static_cast<int16_t>(field_0x1204);
    out.recolorBound = mRecolor.mane.bound();
    out.recolorKey = mRecolor.mane.appliedKey();
    mRecolor.mane.probe(out.mane, out.manePristine);
    out.soundAnims = mSoundAnims;
}

namespace {

int daDummyHorse_create(void* i_this) {
    return static_cast<daDummyHorse_c*>(i_this)->create();
}

int daDummyHorse_delete(void* i_this) {
    static_cast<daDummyHorse_c*>(i_this)->destroy();
    return TRUE;
}

int daDummyHorse_execute(void* i_this) {
    return static_cast<daDummyHorse_c*>(i_this)->execute();
}

int daDummyHorse_isDelete(void*) {
    return TRUE;
}

int daDummyHorse_draw(void* i_this) {
    return static_cast<daDummyHorse_c*>(i_this)->draw();
}

}  // namespace

// Group 5 as daHorse_c: suspend regions never freeze it and nothing searches it to talk or fight.
// Drawn before the dummies: a rider adds itself to its horse's shadow.
const ActorProfileDesc g_dummyHorseProfile = {
    .name = "TThorse",
    .priority_group = 9,
    .process_size = sizeof(daDummyHorse_c),
    .draw_priority = fpcDwPi_HORSE_e,
    .status = fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    .group = fopAc_UNK_GROUP_5_e,
    .cull_type = fopAc_CULLBOX_CUSTOM_e,
    .create_function = daDummyHorse_create,
    .delete_function = daDummyHorse_delete,
    .execute_function = daDummyHorse_execute,
    .is_delete_function = daDummyHorse_isDelete,
    .draw_function = daDummyHorse_draw,
};

}  // namespace twili
