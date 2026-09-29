// PLAYER_UPDATE: our pose once per player tick, as sequenced keyframes and deltas.

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "fx/Fishing.hpp"
#include "fx/ItemFx.hpp"
#include "fx/RemoteTransformFx.hpp"
#include "fx/StatusFx.hpp"
#include "fx/WolfFx.hpp"
#include "horse/HorseSync.hpp"
#include "presence/Presence.hpp"
#include "pvp/Pvp.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRSolidHeap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

namespace twili {

static constexpr uint16_t kNoAnm = 0xFFFF;
static constexpr uint16_t kMaxDemoAnmArcNo = 8;
// Every clip daAlink asks of a demo archive has a resource index of at most 0x2E
static constexpr uint16_t kMaxDemoAnmRes = 0xFF;
static constexpr uint16_t kFirstAlAnmBck = 0x8;
static constexpr uint16_t kLastAlAnmBck = 0x30D;
static bool isValidAnm(uint16_t anm) {
    if (anm == 0 || anm == kNoAnm) {
        return false;
    }

    const uint16_t arcId = (anm >> 12) & 0xF;
    const uint16_t resId = anm & 0xFFF;
    if (resId == 0) {
        return false;
    }
    if (arcId > kMaxDemoAnmArcNo) {
        return false;
    }

    if (arcId != 0) {
        return resId <= kMaxDemoAnmRes;
    }

    if (resId < kFirstAlAnmBck || resId > kLastAlAnmBck) {
        return false;
    }

    JKRArchive* archive = dComIfGp_getAnmArchive();
    return archive && archive->isFileEntry(resId);
}

// daPy_anmHeap_c::loadData's table, indexed by arcNo - 1.
static const char* const kPlayerAnmArcNames[kMaxDemoAnmArcNo] = {
    "alSumou", "B_oh", "TWGate_Lk", "TWGate_Wf", "B_DR", "Lv6Gate", "B_gnd", "B_mgn",
};

// The lookup of dRes_control_c::getRes without its reports
void* Session::residentPlayerArcAnm(uint16_t arcNo, uint16_t resIdx) {
    if (arcNo < 1 || arcNo > kMaxDemoAnmArcNo) {
        return nullptr;
    }
    dRes_info_c* info = dComIfG_getObjectResInfo(kPlayerAnmArcNames[arcNo - 1]);
    // No archive while its load is still under way.
    JKRArchive* archive = info != nullptr ? info->getArchive() : nullptr;
    if (archive == nullptr || resIdx >= archive->countFile()) {
        return nullptr;
    }
    // A garbage id can name a model or collision entry
    JKRArchive::SDIDirEntry* node = archive->mNodes;
    for (s32 i = 0; i < archive->countDirectory(); i++, node++) {
        const u32 first = node->first_file_index;
        if (resIdx >= first && resIdx < first + node->num_entries) {
            return node->type == 'BCK ' || node->type == 'BCKS' ? info->getRes(resIdx)
                                                                : nullptr;
        }
    }
    return nullptr;
}

static uint16_t sanitizeAnm(uint16_t anm) {
    return isValidAnm(anm) ? anm : 0;
}

static void sanitizeAnmPack(uint16_t anms[3], float ratios[3]) {
    for (int i = 0; i < 3; i++) {
        const uint16_t sanitized = sanitizeAnm(anms[i]);
        if (sanitized != anms[i]) {
            anms[i] = sanitized;
            ratios[i] = 0.0f;
        }
    }
}

static bool anmHeapOwns(const daPy_anmHeap_c& heap, const void* anm) {
    JKRSolidHeap* solid = heap.mAnimeHeap;
    return solid != nullptr && anm >= solid->getStartAddr() && anm < solid->getEndAddr();
}

// Unlike the heap's own arcNo/idx pair, arcNo 0 (the stage demo archive) has no wire encoding
static uint16_t heapAnmId(const daPy_anmHeap_c& heap) {
    const uint16_t idx = heap.getIdx();
    const uint16_t arcNo = heap.getArcNo();
    if (idx == 0 || idx == kNoAnm) {
        return 0;
    }
    if (arcNo == kNoAnm) {
        return idx;
    }
    if (arcNo >= 1 && arcNo <= kMaxDemoAnmArcNo) {
        return static_cast<uint16_t>((arcNo << 12) | (idx & 0x0FFF));
    }
    return 0;
}

uint16_t Session::playerPackAnmId(daAlink_c& link, bool upper, int pack) {
    J3DAnmTransform* bck = (upper ? link.mNowAnmPackUpper[pack] : link.mNowAnmPackUnder[pack])
                               .getAnmTransform();
    if (bck == nullptr) {
        return 0;
    }
    const daPy_anmHeap_c& own = upper ? link.mUpperAnmHeap[pack] : link.mUnderAnmHeap[pack];
    const u16 jointNum = link.mpLinkModel->getModelData()->getJointNum();
    if (bck->field_0x1e != jointNum) {
        static uint16_t sLoggedUnfit = kNoAnm;
        if (own.getIdx() != sLoggedUnfit) {
            sLoggedUnfit = own.getIdx();
            TwiliLog.debug("[presence] {} pack {} clip 0x{:X} has {} tracks, body has {}",
                          upper ? "upper" : "lower", pack, own.getIdx(), bck->field_0x1e,
                          jointNum);
        }
        return 0;
    }
    if (anmHeapOwns(own, bck)) {
        return heapAnmId(own);
    }
    for (int i = 0; i < 3; i++) {
        if (anmHeapOwns(link.mUnderAnmHeap[i], bck) || anmHeapOwns(link.mUpperAnmHeap[i], bck)) {
            return 0;
        }
    }
    if (upper && bck == link.mNowAnmPackUnder[pack].getAnmTransform()) {
        return 0;
    }
    const uint16_t arcNo = own.getArcNo();
    if (arcNo == kNoAnm || arcNo == 0) {
        return 0;
    }
    // Only when the name is what plays
    if (residentPlayerArcAnm(arcNo, own.getIdx() & 0x0FFF) != bck) {
        static uint16_t sLoggedStale = kNoAnm;
        if (own.getIdx() != sLoggedStale) {
            sLoggedStale = own.getIdx();
            TwiliLog.debug("[presence] {} pack {} names clip 0x{:X} of archive {}, which it "
                          "does not play", upper ? "upper" : "lower", pack, own.getIdx(), arcNo);
        }
        return 0;
    }
    return heapAnmId(own);
}

// The AlAnm index a Midna anm heap holds, 0 for a demo archive clip or none.
static uint16_t midnaAnmId(const daPy_anmHeap_c& heap) {
    return heap.checkNoSetArcNo() && !heap.checkNoSetIdx() ? heap.getIdx() : 0;
}

// J3DShpFlag_Visible set means hidden (J3DShape::hide).
static bool shapeShown(J3DModel* model, u16 material) {
    J3DModelData* data = model != nullptr ? model->getModelData() : nullptr;
    if (data == nullptr || material >= data->getMaterialNum()) {
        return false;
    }
    J3DShape* shape = data->getMaterialNodePointer(material)->getShape();
    return shape != nullptr && !shape->checkFlag(J3DShpFlag_Visible);
}

// Reads the daMidna_c of the previous tick
bool Session::captureLocalMidna(bool inCutscene, RemoteMidnaPose& out) {
    out = RemoteMidnaPose{};
    daAlink_c* link = daAlink_getAlinkActorClass();
    daMidna_c* m = daPy_py_c::getMidnaActor();
    if (link == nullptr || m == nullptr || link->getClothesChangeWaitTimer() != 0) {
        return false;
    }
    const u32 demoMode = m->mDemoMode;
    // setMatrix's getWolfMidnaMatrix branch (a transformation's demo leaves her there a while)
    const bool onBack = link->checkWolf() && !m->checkWolfNoPos() && demoMode != 9 &&
                        demoMode != 0x200 && m->mpKago == nullptr &&
                        !m->checkShadowModelDrawSmode();
    if (onBack && (inCutscene || !link->checkMidnaRide())) {
        return false;
    }
    if (!onBack && (m->mpKago != nullptr || demoMode == 0x200 || m->checkShadowModelDrawSmode())) {
        return false;
    }
    // An AlAnm clip, on the wolf's own md.bmd (initMidnaModel) or, off the back, on her shadow
    const uint16_t body = midnaAnmId(m->mBckHeap[0]);
    J3DModel* md = m->mpModel;
    const bool realBody = md != nullptr && md == link->getMidnaModel() && !m->checkNoDraw() &&
                          !m->checkShadowModelDrawDemoForce();
    if (body == 0 || (onBack && (md == nullptr || md != link->getMidnaModel()))) {
        return false;
    }
    // daMidna_c::draw's md.bmd branch, and the wolf's real shadow in daAlink_c::draw.
    const bool visible = !m->checkNoDrawState() && !link->checkPlayerNoDraw();
    const bool drawn = visible && realBody;
    const bool shadow = !daAlink_c::checkCloudSea() && !m->checkShadowNoDraw() &&
                        !m->checkShadowModelDraw();
    if (onBack ? !drawn && !shadow : !visible) {
        return false;
    }

    out.mode = !onBack ? kMidnaApart : drawn ? kMidnaDrawn : kMidnaShadowOnly;
    if (!shadow || !onBack) {
        out.flags |= kMidnaNoShadow;
    }
    if (!onBack && !realBody) {
        out.flags |= kMidnaSilhouette;
    }
    if (!onBack) {
        MtxP base = m->mpShadowModel->getBaseTRMtx();
        csXyz rot;
        mDoMtx_MtxToRot(base, &rot);
        for (int i = 0; i < 3; i++) {
            out.worldPos[i] = base[i][3];
        }
        out.worldAngle[0] = rot.x;
        out.worldAngle[1] = rot.y;
        out.worldAngle[2] = rot.z;
    }
    // setEyeMove's offsets, as daMidna_matAnm_c::calc applies them
    if (daMidna_matAnm_c::getEyeMoveFlg()) {
        out.eyeFlags |= kMidnaEyeMove;
    }
    out.eyeFlags |= (std::min<u8>(daMidna_matAnm_c::getMorfFrame(), 7) << kMidnaEyeMorfShift);
    for (int i = 0; i < 2; i++) {
        if (m->mpEyeMatAnm[i] != nullptr) {
            out.eyeOffset[i * 2] = m->mpEyeMatAnm[i]->mNowOffsetX;
            out.eyeOffset[i * 2 + 1] = m->mpEyeMatAnm[i]->mNowOffsetY;
        }
    }
    out.bodyBck = body;
    out.bodyFrame = m->mpMorf->getFrame();
    // setAnm usually loads the body clip into the upper heap too (both play from frame 0)
    const uint16_t upper = midnaAnmId(m->mBckHeap[1]);
    const float upperFrame = m->mUpperBck.getFrame();
    if (upper != 0 &&
        (upper != body || std::fabs(upperFrame - out.bodyFrame) >= 1.0f / kFrameScale))
    {
        out.upperBck = upper;
        out.upperFrame = upperFrame;
    }
    if (m->checkStateFlg0(daMidna_c::FLG0_UNK_800000)) {  // a face layer is on (setFaceAnime)
        out.faceBck = midnaAnmId(m->mBckHeap[2]);
        out.faceFrame = out.faceBck != 0 ? m->mFaceBck.getFrame() : 0.0f;
    }
    out.btp = midnaAnmId(m->mBtpHeap);
    out.btk = midnaAnmId(m->mBtkHeap);
    // 0xFD is a demo archive hand, which never shows on the back.
    auto hand = [](u16 i) -> uint8_t { return i >= 0xFD ? 0xFE : static_cast<uint8_t>(i); };
    out.leftHand = hand(m->mLeftHandShapeIdx);
    out.rightHand = hand(m->mRightHandShapeIdx);
    // setBodyPartMatrix shows one of the hair hand's three materials.
    J3DModel* hair = link->getMidnaHairHandModel();
    out.hairHand = shapeShown(hair, 2) ? 2 : shapeShown(hair, 1) ? 1 : 0;
    if ((daMidna_c::checkMidnaTired() || m->checkForceTiredColor()) &&
        !m->checkForceNormalColor())
    {
        out.flags |= kMidnaTired;
    }
    if (m->mJntNo == 5 /* JNT_CHIN */) {
        out.flags |= kMidnaFaceFromChin;
    }
    if (m->checkNoHairScale()) {
        out.flags |= kMidnaNoHairScale;
    }
    if (m->checkStateFlg0(static_cast<daMidna_c::daMidna_FLG0>(daMidna_c::FLG0_UNK_200000 |
                                                              daMidna_c::FLG0_UNK_10000000)))
    {
        out.flags |= kMidnaHairFromBck;
    }
    out.neckX = m->mNeckAngle.x;
    out.neckY = m->mNeckAngle.y;
    out.backboneZ = m->mBackboneAngleZ;
    out.hairTipY = m->mHairAngleY[4];
    out.hairTipZ = m->mHairAngleZ[4];
    return true;
}

static constexpr uint32_t kKeyframeInterval = 30;
// While nothing changes, still send this often
static constexpr uint32_t kHeartbeatInterval = 5;
static constexpr const char* kItemFxSlotKeys[kItemFxSlots] = {"x0", "x1", "x2", "x3",
                                                              "x4", "x5", "x6", "x7"};

// Sender side of the pose stream.
struct PoseSender {
    uint32_t selfClientId = 0;
    uint32_t seq = 0;  // +1 per local player tick while connected, sent or not
    uint32_t lastKeyframeSeq = 0;
    uint32_t lastSentSeq = 0;
    fpc_ProcID playerProcId = fpcM_ERROR_PROCESS_ID_e;
    uint64_t peerSignature = 0;
    uint8_t epoch = 0;
    uint8_t sentFlags = 0;
    int8_t layer = -127;
    char stage[8] = {};
    float lastPos[3] = {};
    bool havePrev = false;
    // A receiver may have dropped the stream since our last packet (or never had it)
    bool needKeyframe = true;
    // The direction of the transformation running (captureTransformFx).
    bool tfLatched = false;
    bool tfToWolf = false;
    WirePose sent;
};

static PoseSender s_poseTx;

uint32_t Session::localPoseSeq() {
    return s_poseTx.seq;
}

// procCoMetamorphose as setMetamorphoseEffect and draw() see it this tick
static void captureTransformFx(PoseSender& tx, RemoteTransformFx& out) {
    out = RemoteTransformFx{};
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || !link->checkMetamorphose()) {
        tx.tfLatched = false;
        return;
    }
    const bool post = link->mProcVar5.field_0x3012 != 0;
    if (!tx.tfLatched) {
        // The body flips on the tick field_0x3012 is set, so this holds either way
        tx.tfToWolf = post ? link->checkWolf() != 0 : link->checkWolf() == 0;
        tx.tfLatched = true;
    }
    out.flags = kTfActive | (post ? kTfPostSwap : 0) | (tx.tfToWolf ? kTfToWolf : 0);
    out.tev = link->mProcVar3.field_0x300e;
    out.hatScale = link->field_0x347c;
    out.anchor[0] = link->field_0x37c8.x;
    out.anchor[1] = link->field_0x37c8.y;
    out.anchor[2] = link->field_0x37c8.z;
}

void Session::captureLocalTransformFx(RemoteTransformFx& out) {
    captureTransformFx(s_poseTx, out);
}

// setPlayerUpdateTestPatch
static nlohmann::json s_testPatch;
static int s_testPatchPackets = 0;

void Session::setPlayerUpdateTestPatch(const nlohmann::json& patch, int packets) {
    s_testPatch = patch;
    s_testPatchPackets = packets;
}

static int32_t quantize(float value, float scale) {
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp<long long>(
        std::llround(static_cast<double>(value) * scale), -2000000000LL, 2000000000LL));
}

template <size_t N>
static void putField(nlohmann::json& packet, const char* key, const int32_t (&cur)[N],
                     const int32_t (&prev)[N], bool all, bool& changed) {
    if (!all && std::equal(cur, cur + N, prev)) {
        return;
    }
    packet[key] = std::vector<int32_t>(cur, cur + N);
    changed = true;
}

static void putField(nlohmann::json& packet, const char* key, int32_t cur, int32_t prev,
                     bool all, bool& changed) {
    if (!all && cur == prev) {
        return;
    }
    packet[key] = cur;
    changed = true;
}

template <size_t N>
static bool anyNonZero(const int32_t (&v)[N]) {
    return std::any_of(v, v + N, [](int32_t x) { return x != 0; });
}

// All zero while she is not shown, as a receiver's groups are after a keyframe without "md".
static void encodeMidna(const RemoteMidnaPose& m, WirePose& w) {
    if (m.mode == kMidnaNone) {
        return;
    }
    w.md[0] = m.mode | (m.hairHand << 2) | (m.flags << 4);
    w.md[1] = m.bodyBck;
    w.md[2] = m.upperBck;
    w.md[3] = m.faceBck;
    w.md[4] = m.btp;
    w.md[5] = m.btk;
    w.md[6] = m.leftHand | (m.rightHand << 8);
    w.mf[0] = quantize(m.bodyFrame, kFrameScale);
    w.mf[1] = quantize(m.upperFrame, kFrameScale);
    w.mf[2] = quantize(m.faceFrame, kFrameScale);
    w.ma[0] = m.neckX;
    w.ma[1] = m.neckY;
    w.ma[2] = m.backboneZ;
    w.ma[3] = m.hairTipY;
    w.ma[4] = m.hairTipZ;
    w.me[0] = m.eyeFlags;
    for (int i = 0; i < 4; i++) {
        w.me[1 + i] = quantize(m.eyeOffset[i], kRatioScale);
    }
    if (m.mode == kMidnaApart) {
        for (int i = 0; i < 3; i++) {
            w.mw[i] = quantize(m.worldPos[i], kPosScale);
            w.mw[3 + i] = m.worldAngle[i];
        }
    }
}

void Session::sendPlayerUpdate(float posX, float posY, float posZ,
                              int16_t angleX, int16_t angleY, int16_t angleZ,
                              int16_t shapeAngleX, int16_t shapeAngleY, int16_t shapeAngleZ,
                              int16_t bodyAngleX, int16_t bodyAngleY, int16_t bodyAngleZ,
                              int16_t bodyTwistY, bool attentionLock, bool shieldInHand,
                              int8_t transformStatus, bool modelSwap,
                              const uint16_t upperANMs[3],
                              const uint16_t lowerANMs[3], const float upperFrames[3],
                              const float lowerFrames[3], const float upperRatios[3],
                              const float lowerRatios[3],
                              const int16_t waterDropColors[2][4],
                              const int16_t swordUpColors[2][4], uint8_t swordItem,
                              uint8_t shieldItem, uint8_t clothesItem, uint16_t equipItem,
                              uint8_t upperBlendMode, float upperBlendRatio, uint8_t cutType,
                              bool swordBlurActive, uint8_t swordBlurAlpha,
                              uint8_t leftHandIndex, uint8_t rightHandIndex,
                              uint8_t leftHandItemOverride, uint8_t rightHandItemOverride,
                              uint8_t leftHandGripOverride, uint8_t rightHandGripOverride,
                              uint16_t leftItemJoint, uint16_t rightItemJoint,
                              uint16_t itemBckId, float itemBckFrame,
                              bool itemAmmoLoaded, uint16_t itemProjectileSeq,
                              uint8_t itemProjectileType, bool swordChargeActive,
                              float swordChargeFrame, uint16_t visFlags) {
    PoseSender& tx = s_poseTx;
    tx.seq++;
    if (tx.selfClientId != mSelfClientId) {
        const uint32_t seq = tx.seq;
        tx = PoseSender{};
        tx.seq = seq;
        tx.selfClientId = mSelfClientId;
        statusfx::resetSender();
        horse::resetSender();
    }
    // Every tick, sent or not, so the cutscene release delay runs.
    const bool inCutscene = localCutsceneRunning();
    // Every tick too, so the direction is latched on the transformation's first tick.
    RemoteTransformFx transformFx;
    captureTransformFx(tx, transformFx);
    // Every tick too, so its one-shot generations see every edge.
    RemoteStatusFx status;
    statusfx::captureLocal(status);
    // And so its jump counter sees every jump.
    wolffx::track();

    const char* stage = dComIfGp_getStartStageName();
    if (!isSaveLoaded() || stage == nullptr) {
        tx.needKeyframe = true;
        return;
    }
    const int8_t layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    const daPy_py_c* link = dComIfGp_getLinkPlayer();
    const fpc_ProcID procId = link != nullptr ? fopAcM_GetID(link) : fpcM_ERROR_PROCESS_ID_e;
    const float dx = posX - tx.lastPos[0];
    const float dy = posY - tx.lastPos[1];
    const float dz = posZ - tx.lastPos[2];
    if (!tx.havePrev || procId != tx.playerProcId || layer != tx.layer ||
        std::strncmp(stage, tx.stage, sizeof(tx.stage)) != 0 ||
        dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance)
    {
        if (tx.havePrev) {
            tx.epoch++;
        }
        tx.needKeyframe = true;
        tx.playerProcId = procId;
        tx.layer = layer;
        std::memset(tx.stage, 0, sizeof(tx.stage));
        std::strncpy(tx.stage, stage, sizeof(tx.stage) - 1);
    }
    tx.lastPos[0] = posX;
    tx.lastPos[1] = posY;
    tx.lastPos[2] = posZ;
    tx.havePrev = true;

    if (std::strncmp(stage, mLastStageName, sizeof(mLastStageName)) != 0 ||
        layer != mLastLayerNo)
    {
        tx.needKeyframe = true;
        return;
    }

    // Peers who can use the stream (same layer, same protocol).
    bool anyPeer = false;
    uint64_t peers = 1469598103934665603ull;
    for (const auto& [id, c] : mClients) {
        if (!clientIsInCurrentLayer(c) || c.protocolVersion != kProtocolVersion) {
            continue;
        }
        anyPeer = true;
        peers = (peers ^ ((static_cast<uint64_t>(id) << 8) | static_cast<uint8_t>(c.saveTblNo))) *
                1099511628211ull;
    }
    if (!anyPeer) {
        tx.needKeyframe = true;
        return;
    }

    uint16_t safeUpperANMs[3] = {upperANMs[0], upperANMs[1], upperANMs[2]};
    uint16_t safeLowerANMs[3] = {lowerANMs[0], lowerANMs[1], lowerANMs[2]};
    float safeUpperRatios[3] = {upperRatios[0], upperRatios[1], upperRatios[2]};
    float safeLowerRatios[3] = {lowerRatios[0], lowerRatios[1], lowerRatios[2]};
    sanitizeAnmPack(safeUpperANMs, safeUpperRatios);
    sanitizeAnmPack(safeLowerANMs, safeLowerRatios);

    WirePose cur;
    cur.p[0] = quantize(posX, kPosScale);
    cur.p[1] = quantize(posY, kPosScale);
    cur.p[2] = quantize(posZ, kPosScale);
    cur.a[0] = angleX;
    cur.a[1] = angleY;
    cur.a[2] = angleZ;
    cur.sa[0] = shapeAngleX;
    cur.sa[1] = shapeAngleY;
    cur.sa[2] = shapeAngleZ;
    cur.ba[0] = bodyAngleX;
    cur.ba[1] = bodyAngleY;
    cur.ba[2] = bodyAngleZ;
    cur.tw = bodyTwistY;
    for (int i = 0; i < 3; i++) {
        cur.la[i] = safeLowerANMs[i];
        cur.ua[i] = safeUpperANMs[i];
        cur.lf[i] = quantize(lowerFrames[i], kFrameScale);
        cur.uf[i] = quantize(upperFrames[i], kFrameScale);
        cur.lr[i] = quantize(safeLowerRatios[i], kRatioScale);
        cur.ur[i] = quantize(safeUpperRatios[i], kRatioScale);
    }
    for (int i = 0; i < 8; i++) {
        cur.wd[i] = waterDropColors[i / 4][i % 4];
        cur.su[i] = swordUpColors[i / 4][i % 4];
    }
    cur.eq[0] = clothesItem;
    cur.eq[1] = swordItem;
    cur.eq[2] = shieldItem;
    cur.eq[3] = equipItem;
    cur.ub[0] = upperBlendMode;
    cur.ub[1] = quantize(upperBlendRatio, kRatioScale);
    cur.ct = cutType;
    cur.bl = swordBlurAlpha;
    cur.hi[0] = leftHandIndex;
    cur.hi[1] = rightHandIndex;
    cur.hi[2] = leftHandItemOverride;
    cur.hi[3] = rightHandItemOverride;
    cur.hi[4] = leftHandGripOverride;
    cur.hi[5] = rightHandGripOverride;
    cur.ij[0] = leftItemJoint;
    cur.ij[1] = rightItemJoint;
    cur.ib[0] = itemBckId;
    cur.ib[1] = quantize(itemBckFrame, kFrameScale);
    cur.pr[0] = itemProjectileSeq;
    cur.pr[1] = itemProjectileType;
    cur.cf = quantize(swordChargeFrame, kFrameScale);
    cur.vf = visFlags | itemfx::localVisFlags();
    RemoteMidnaPose midna;
    captureLocalMidna(inCutscene, midna);  // all none when she is not shown
    encodeMidna(midna, cur);
    encodeTransformFx(transformFx, cur.tf);
    statusfx::encode(status, cur.sx, cur.sf);
    RemoteWolfFx wolfFx;
    wolffx::captureLocal(midna, wolfFx);  // not gated on inCutscene: receivers hide the dummy
    wolffx::encode(wolfFx, cur.wx);
    RemoteItemFx itemFx;
    ItemFxEvent itemEvents[itemfx::kMaxEventsPerPacket];
    const int itemEventCount = itemfx::captureLocal(tx.seq, itemFx, itemEvents);
    for (int i = 0; i < kItemFxSlots; i++) {
        itemfx::encodeSlot(itemFx.slots[i], cur.x[i]);
    }
    itemfx::encodeHookshot(itemFx.hk, cur.hk);
    itemfx::encodeIronBall(itemFx.bc, cur.bc);
    itemfx::encodeLevelSfx(itemFx.ls, cur.ls);
    itemfx::encodeHeldExtra(itemFx.hx, cur.hx);
    fishing::captureLocal(daAlink_getAlinkActorClass(), itemFx.fr);
    fishing::encode(itemFx.fr, cur.fr);
    RemoteHorsePose horsePose;
    horse::captureLocal(horsePose);
    horse::encode(horsePose, cur);

    uint8_t flags = 0;
    if (attentionLock) flags |= kPresenceAttentionLock;
    if (shieldInHand) flags |= kPresenceShieldInHand;
    if (swordBlurActive) flags |= kPresenceSwordBlur;
    if (itemAmmoLoaded) flags |= kPresenceAmmoLoaded;
    if (swordChargeActive) flags |= kPresenceSwordCharge;
    if (transformStatus == 1) flags |= kPresenceWolf;
    if (inCutscene) flags |= kPresenceInCutscene;
    if (modelSwap) flags |= kPresenceModelSwap;

    // A patched packet stands on its own, so its values never outlive it on the receiver.
    const bool patched = s_testPatchPackets > 0;
    const bool keyframe = tx.needKeyframe || peers != tx.peerSignature || patched ||
                          tx.seq - tx.lastKeyframeSeq >= kKeyframeInterval;
    nlohmann::json packet = {
        {"type", "PLAYER_UPDATE"}, {"quiet", true}, {"seq", tx.seq},
        {"ep", tx.epoch},          {"fl", flags},
    };
    bool changed = false;
    const WirePose& o = tx.sent;
    putField(packet, "p", cur.p, o.p, keyframe, changed);
    putField(packet, "a", cur.a, o.a, keyframe, changed);
    putField(packet, "sa", cur.sa, o.sa, keyframe, changed);
    putField(packet, "ba", cur.ba, o.ba, keyframe, changed);
    putField(packet, "tw", cur.tw, o.tw, keyframe, changed);
    putField(packet, "la", cur.la, o.la, keyframe, changed);
    putField(packet, "ua", cur.ua, o.ua, keyframe, changed);
    putField(packet, "lf", cur.lf, o.lf, keyframe, changed);
    putField(packet, "uf", cur.uf, o.uf, keyframe, changed);
    putField(packet, "lr", cur.lr, o.lr, keyframe, changed);
    putField(packet, "ur", cur.ur, o.ur, keyframe, changed);
    putField(packet, "wd", cur.wd, o.wd, keyframe, changed);
    putField(packet, "su", cur.su, o.su, keyframe, changed);
    putField(packet, "eq", cur.eq, o.eq, keyframe, changed);
    putField(packet, "ub", cur.ub, o.ub, keyframe, changed);
    putField(packet, "ct", cur.ct, o.ct, keyframe, changed);
    putField(packet, "bl", cur.bl, o.bl, keyframe, changed);
    putField(packet, "hi", cur.hi, o.hi, keyframe, changed);
    putField(packet, "ij", cur.ij, o.ij, keyframe, changed);
    putField(packet, "ib", cur.ib, o.ib, keyframe, changed);
    putField(packet, "pr", cur.pr, o.pr, keyframe, changed);
    putField(packet, "cf", cur.cf, o.cf, keyframe, changed);
    putField(packet, "vf", cur.vf, o.vf, keyframe, changed);
    putField(packet, "tf", cur.tf, o.tf, keyframe, changed);
    // Free for players without Midna
    if (!keyframe || cur.md[0] != 0) {
        putField(packet, "md", cur.md, o.md, keyframe, changed);
        putField(packet, "mf", cur.mf, o.mf, keyframe, changed);
        putField(packet, "ma", cur.ma, o.ma, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.me)) {
        putField(packet, "me", cur.me, o.me, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.mw)) {
        putField(packet, "mw", cur.mw, o.mw, keyframe, changed);
    }
    // The same for a player nothing is wrong with.
    if (!keyframe || anyNonZero(cur.sx)) {
        putField(packet, "sx", cur.sx, o.sx, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.sf)) {
        putField(packet, "sf", cur.sf, o.sf, keyframe, changed);
    }
    // And for a wolf that never attacked.
    if (!keyframe || anyNonZero(cur.wx)) {
        putField(packet, "wx", cur.wx, o.wx, keyframe, changed);
    }
    // And for empty item slots, a clawshot at rest, a ball in the hand, silence and plain items.
    for (int i = 0; i < kItemFxSlots; i++) {
        if (!keyframe || anyNonZero(cur.x[i])) {
            putField(packet, kItemFxSlotKeys[i], cur.x[i], o.x[i], keyframe, changed);
        }
    }
    if (!keyframe || anyNonZero(cur.hk)) {
        putField(packet, "hk", cur.hk, o.hk, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.bc)) {
        putField(packet, "bc", cur.bc, o.bc, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.ls)) {
        putField(packet, "ls", cur.ls, o.ls, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.hx)) {
        putField(packet, "hx", cur.hx, o.hx, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.fr)) {
        putField(packet, "fr", cur.fr, o.fr, keyframe, changed);
    }
    // And for a player without a horse in sight.
    if (!keyframe || cur.hsx[0] != 0) {
        putField(packet, "ox", cur.hsx, o.hsx, keyframe, changed);
        putField(packet, "op", cur.hsp, o.hsp, keyframe, changed);
        putField(packet, "os", cur.hss, o.hss, keyframe, changed);
        putField(packet, "oa", cur.hsa, o.hsa, keyframe, changed);
        putField(packet, "of", cur.hsf, o.hsf, keyframe, changed);
        putField(packet, "ow", cur.hsw, o.hsw, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.hsk)) {
        putField(packet, "ok", cur.hsk, o.hsk, keyframe, changed);
    }
    if (!keyframe || cur.hsr[0] != 0) {
        putField(packet, "or", cur.hsr, o.hsr, keyframe, changed);
    }
    if (!keyframe || anyNonZero(cur.hsz)) {
        putField(packet, "oz", cur.hsz, o.hsz, keyframe, changed);
    }
    if (keyframe) {
        packet["xv"] = 1;
    }
    if (itemEventCount > 0) {
        nlohmann::json events = nlohmann::json::array();
        for (int i = 0; i < itemEventCount; i++) {
            int32_t ev[7];
            itemfx::encodeEvent(itemEvents[i], ev);
            events.push_back(std::vector<int32_t>(ev, ev + 7));
        }
        packet["xe"] = std::move(events);
        changed = true;
    }
    if (!keyframe && !changed && flags == tx.sentFlags &&
        tx.seq - tx.lastSentSeq < kHeartbeatInterval)
    {
        return;
    }
    if (keyframe) {
        packet["k"] = 1;
        tx.lastKeyframeSeq = tx.seq;
        tx.needKeyframe = false;
        tx.peerSignature = peers;
    }
    if (patched) {
        packet.merge_patch(s_testPatch);
        // The first unpatched packet puts every field back.
        tx.needKeyframe = --s_testPatchPackets == 0;
    }
    tx.sent = cur;
    tx.sentFlags = flags;
    tx.lastSentSeq = tx.seq;
    // A dropped packet leaves the receivers' base behind: the next one must stand alone.
    if (!send(packet, net::Delivery::Droppable)) {
        tx.needKeyframe = true;
    }
}

template <size_t N>
static void takeField(const nlohmann::json& packet, const char* key, int32_t (&out)[N]) {
    const auto it = packet.find(key);
    if (it == packet.end() || !it->is_array() || it->size() != N) {
        return;
    }
    int32_t values[N];
    for (size_t i = 0; i < N; i++) {
        if (!(*it)[i].is_number_integer()) {  // true for unsigned too
            return;
        }
        values[i] = static_cast<int32_t>((*it)[i].get<int64_t>());
    }
    std::copy(values, values + N, out);
}

static void takeField(const nlohmann::json& packet, const char* key, int32_t& out) {
    const auto it = packet.find(key);
    if (it != packet.end() && it->is_number_integer()) {
        out = static_cast<int32_t>(it->get<int64_t>());
    }
}

// Ids are only range-checked here
static void decodeMidna(const WirePose& w, RemoteMidnaPose& m) {
    m = RemoteMidnaPose{};  // nothing stale survives mode 0
    const uint32_t head = static_cast<uint32_t>(w.md[0]);
    const uint8_t mode = head & 3;
    if (mode == kMidnaNone) {
        return;
    }
    if (mode == kMidnaApart) {
        // Off the back she stays near her wolf; anything else is junk.
        float distSq = 0.0f;
        for (int i = 0; i < 3; i++) {
            m.worldPos[i] = w.mw[i] / kPosScale;
            m.worldAngle[i] = static_cast<int16_t>(w.mw[3 + i]);
            const float d = m.worldPos[i] - w.p[i] / kPosScale;
            distSq += d * d;
        }
        if (!(distSq <= kMaxMidnaApartDist * kMaxMidnaApartDist)) {
            m = RemoteMidnaPose{};
            return;
        }
    }
    m.mode = mode;
    m.eyeFlags = static_cast<uint8_t>(w.me[0] & 0x0F);
    for (int i = 0; i < 4; i++) {
        m.eyeOffset[i] = std::clamp(w.me[1 + i] / kRatioScale, -1.0f, 1.0f);
    }
    m.hairHand = (std::min)(static_cast<uint8_t>((head >> 2) & 3), static_cast<uint8_t>(2));
    m.flags = (head >> 4) & 0x3F;
    m.bodyBck = static_cast<uint16_t>(w.md[1]);
    m.upperBck = static_cast<uint16_t>(w.md[2]);
    m.faceBck = static_cast<uint16_t>(w.md[3]);
    m.btp = static_cast<uint16_t>(w.md[4]);
    m.btk = static_cast<uint16_t>(w.md[5]);
    m.leftHand = static_cast<uint8_t>(w.md[6] & 0xFF);
    m.rightHand = static_cast<uint8_t>((w.md[6] >> 8) & 0xFF);
    m.bodyFrame = w.mf[0] / kFrameScale;
    m.upperFrame = w.mf[1] / kFrameScale;
    m.faceFrame = w.mf[2] / kFrameScale;
    m.neckX = static_cast<int16_t>(w.ma[0]);
    m.neckY = static_cast<int16_t>(w.ma[1]);
    m.backboneZ = static_cast<int16_t>(w.ma[2]);
    m.hairTipY = static_cast<int16_t>(w.ma[3]);
    m.hairTipZ = static_cast<int16_t>(w.ma[4]);
}

// When each stage of the remote's last transformation reached us, in its ticks (autotest).
static void traceTransformFx(Client& c, const RemoteTransformFx& prevTf,
                             uint16_t prevClip, bool prevSwap, int8_t prevForm, uint32_t seq) {
    TransformFxTrace& t = c.transformTrace;
    const RemoteTransformFx& tf = c.transformFx;
    auto begin = [&t](bool toWolf) {
        const uint16_t count = t.count + 1;
        t = TransformFxTrace{};
        t.count = count;
        t.toWolf = toWolf;
    };
    if (tf.active() && !prevTf.active()) {
        begin(tf.toWolf());
        t.startSeq = seq;
    }
    // The old body's clip (setSingleAnimeBase / setSingleAnimeWolfBase in procCoMetamorphoseInit).
    const uint16_t clip = c.lowerANMs[0];
    if ((clip == dRes_ID_ALANM_BCK_CHANGEATOW_e || clip == dRes_ID_ALANM_BCK_WL_CHANGEWTOA_e) &&
        clip != prevClip && t.clipSeq == 0)
    {
        t.clipSeq = seq;
    }
    if (t.count == 0) {
        return;
    }
    if (tf.active() && c.modelSwap && !prevSwap && t.swapSeq == 0) {
        t.swapSeq = seq;
    }
    if (tf.postSwap() && !prevTf.postSwap() && t.postSeq == 0) {
        t.postSeq = seq;
    }
    if (c.transformStatus != prevForm && t.flipSeq == 0) {
        t.flipSeq = seq;
    }
    if (tf.active()) {
        t.endFrame = c.lowerFrames[0];
    } else if (prevTf.active() && t.endSeq == 0) {
        t.endSeq = seq;
    }
}

void Session::handlePlayerUpdate(const nlohmann::json& packet) {
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == mSelfClientId) return;
    auto it = mClients.find(id);
    if (it == mClients.end()) return;

    auto& c = it->second;
    // Other protocol versions send another PLAYER_UPDATE format.
    if (c.protocolVersion != kProtocolVersion) return;
    const bool keyframe = packet.value("k", 0) != 0;
    // A delta only means something on top of the keyframe it follows.
    if (!c.hasPlayerUpdate) {
        if (!keyframe) return;
        c.wire = WirePose{};
        c.pose.clear();
        c.transformFx = RemoteTransformFx{};
        c.itemFxEvents.clear();
    }

    WirePose w = c.wire;
    takeField(packet, "p", w.p);
    takeField(packet, "a", w.a);
    takeField(packet, "sa", w.sa);
    takeField(packet, "ba", w.ba);
    takeField(packet, "tw", w.tw);
    takeField(packet, "la", w.la);
    takeField(packet, "ua", w.ua);
    takeField(packet, "lf", w.lf);
    takeField(packet, "uf", w.uf);
    takeField(packet, "lr", w.lr);
    takeField(packet, "ur", w.ur);
    takeField(packet, "wd", w.wd);
    takeField(packet, "su", w.su);
    takeField(packet, "eq", w.eq);
    takeField(packet, "ub", w.ub);
    takeField(packet, "ct", w.ct);
    takeField(packet, "bl", w.bl);
    takeField(packet, "hi", w.hi);
    takeField(packet, "ij", w.ij);
    takeField(packet, "ib", w.ib);
    takeField(packet, "pr", w.pr);
    takeField(packet, "cf", w.cf);
    takeField(packet, "vf", w.vf);
    if (keyframe && packet.find("md") == packet.end()) {
        std::fill(std::begin(w.md), std::end(w.md), 0);
        std::fill(std::begin(w.mf), std::end(w.mf), 0);
        std::fill(std::begin(w.ma), std::end(w.ma), 0);
    } else {
        takeField(packet, "md", w.md);
        takeField(packet, "mf", w.mf);
        takeField(packet, "ma", w.ma);
    }
    if (keyframe && packet.find("me") == packet.end()) {
        std::fill(std::begin(w.me), std::end(w.me), 0);
    } else {
        takeField(packet, "me", w.me);
    }
    if (keyframe && packet.find("mw") == packet.end()) {
        std::fill(std::begin(w.mw), std::end(w.mw), 0);
    } else {
        takeField(packet, "mw", w.mw);
    }
    // Every keyframe of a build that replays transformations carries "tf".
    const bool hasTf = packet.find("tf") != packet.end();
    if (keyframe && !hasTf) {
        std::fill(std::begin(w.tf), std::end(w.tf), 0);
    } else {
        takeField(packet, "tf", w.tf);
    }
    if (keyframe || hasTf) {
        c.sendsTransformFx = hasTf;
    }
    if (keyframe && packet.find("sx") == packet.end()) {
        std::fill(std::begin(w.sx), std::end(w.sx), 0);
    } else {
        takeField(packet, "sx", w.sx);
    }
    if (keyframe && packet.find("sf") == packet.end()) {
        std::fill(std::begin(w.sf), std::end(w.sf), 0);
    } else {
        takeField(packet, "sf", w.sf);
    }
    if (keyframe && packet.find("wx") == packet.end()) {
        std::fill(std::begin(w.wx), std::end(w.wx), 0);
    } else {
        takeField(packet, "wx", w.wx);
    }
    for (int i = 0; i < kItemFxSlots; i++) {
        if (keyframe && packet.find(kItemFxSlotKeys[i]) == packet.end()) {
            std::fill(std::begin(w.x[i]), std::end(w.x[i]), 0);
        } else {
            takeField(packet, kItemFxSlotKeys[i], w.x[i]);
        }
    }
    auto takeItemGroup = [&](const char* key, auto& field) {
        if (keyframe && packet.find(key) == packet.end()) {
            std::fill(std::begin(field), std::end(field), 0);
        } else {
            takeField(packet, key, field);
        }
    };
    takeItemGroup("hk", w.hk);
    takeItemGroup("bc", w.bc);
    takeItemGroup("ls", w.ls);
    takeItemGroup("hx", w.hx);
    takeItemGroup("fr", w.fr);
    takeItemGroup("ox", w.hsx);
    takeItemGroup("op", w.hsp);
    takeItemGroup("os", w.hss);
    takeItemGroup("oa", w.hsa);
    takeItemGroup("of", w.hsf);
    takeItemGroup("ow", w.hsw);
    takeItemGroup("ok", w.hsk);
    takeItemGroup("or", w.hsr);
    takeItemGroup("oz", w.hsz);
    if (keyframe) {
        c.sendsItemFx = packet.find("xv") != packet.end();
    }
    const uint8_t flags = static_cast<uint8_t>(packet.value("fl", 0u));
    c.wire = w;
    const RemoteTransformFx prevTf = c.transformFx;
    const uint16_t prevClip = c.lowerANMs[0];
    const bool prevSwap = c.modelSwap;
    const int8_t prevForm = c.transformStatus;

    c.posX = w.p[0] / kPosScale;
    c.posY = w.p[1] / kPosScale;
    c.posZ = w.p[2] / kPosScale;
    c.angleX = static_cast<int16_t>(w.a[0]);
    c.angleY = static_cast<int16_t>(w.a[1]);
    c.angleZ = static_cast<int16_t>(w.a[2]);
    c.shapeAngleX = static_cast<int16_t>(w.sa[0]);
    c.shapeAngleY = static_cast<int16_t>(w.sa[1]);
    c.shapeAngleZ = static_cast<int16_t>(w.sa[2]);
    c.bodyAngleX = static_cast<int16_t>(w.ba[0]);
    c.bodyAngleY = static_cast<int16_t>(w.ba[1]);
    c.bodyAngleZ = static_cast<int16_t>(w.ba[2]);
    c.bodyTwistY = static_cast<int16_t>(w.tw);
    for (int i = 0; i < 3; i++) {
        c.lowerANMs[i] = static_cast<uint16_t>(w.la[i]);
        c.upperANMs[i] = static_cast<uint16_t>(w.ua[i]);
        c.lowerFrames[i] = w.lf[i] / kFrameScale;
        c.upperFrames[i] = w.uf[i] / kFrameScale;
        c.lowerRatios[i] = w.lr[i] / kRatioScale;
        c.upperRatios[i] = w.ur[i] / kRatioScale;
    }
    sanitizeAnmPack(c.upperANMs, c.upperRatios);
    sanitizeAnmPack(c.lowerANMs, c.lowerRatios);
    for (int i = 0; i < 8; i++) {
        c.waterDropColors[i / 4][i % 4] = static_cast<int16_t>(w.wd[i]);
        c.swordUpColors[i / 4][i % 4] = static_cast<int16_t>(w.su[i]);
    }
    c.clothesItem = static_cast<uint8_t>(w.eq[0]);
    c.swordItem = static_cast<uint8_t>(w.eq[1]);
    c.shieldItem = static_cast<uint8_t>(w.eq[2]);
    c.equipItem = static_cast<uint16_t>(w.eq[3]);
    c.upperBlendMode = static_cast<uint8_t>(w.ub[0]);
    c.upperBlendRatio = w.ub[1] / kRatioScale;
    c.cutType = static_cast<uint8_t>(w.ct);
    c.swordBlurAlpha = static_cast<uint8_t>(w.bl);
    c.leftHandIndex = static_cast<uint8_t>(w.hi[0]);
    c.rightHandIndex = static_cast<uint8_t>(w.hi[1]);
    c.leftHandItemOverride = static_cast<uint8_t>(w.hi[2]);
    c.rightHandItemOverride = static_cast<uint8_t>(w.hi[3]);
    c.leftHandGripOverride = static_cast<uint8_t>(w.hi[4]);
    c.rightHandGripOverride = static_cast<uint8_t>(w.hi[5]);
    c.leftItemJoint = static_cast<uint16_t>(w.ij[0]);
    c.rightItemJoint = static_cast<uint16_t>(w.ij[1]);
    c.itemBckId = static_cast<uint16_t>(w.ib[0]);
    c.itemBckFrame = w.ib[1] / kFrameScale;
    c.itemProjectileSeq = static_cast<uint16_t>(w.pr[0]);
    c.itemProjectileType = static_cast<uint8_t>(w.pr[1]);
    c.swordChargeFrame = w.cf / kFrameScale;
    c.visFlags = static_cast<uint16_t>(w.vf);
    c.attentionLock = (flags & kPresenceAttentionLock) != 0;
    c.shieldInHand = (flags & kPresenceShieldInHand) != 0;
    c.swordBlurActive = (flags & kPresenceSwordBlur) != 0;
    c.itemAmmoLoaded = (flags & kPresenceAmmoLoaded) != 0;
    c.swordChargeActive = (flags & kPresenceSwordCharge) != 0;
    c.transformStatus = (flags & kPresenceWolf) != 0 ? 1 : 0;
    c.modelSwap = (flags & kPresenceModelSwap) != 0;
    c.presenceFlags = flags;
    decodeMidna(w, c.midna);
    c.transformFx = decodeTransformFx(w.tf, c.posX, c.posY, c.posZ);
    c.status = statusfx::decode(w.sx, w.sf);
    c.wolfFx = wolffx::decode(w.wx);
    if (c.midna.mode != kMidnaNone && (c.wolfFx.flags & kWolfFxHairAim)) {
        c.midna.hairAimValid = true;
        c.midna.hairAim = c.wolfFx.hairAim;
    }
    for (int i = 0; i < kItemFxSlots; i++) {
        c.itemFx.slots[i] = itemfx::decodeSlot(w.x[i]);
    }
    c.itemFx.hk = itemfx::decodeHookshot(w.hk);
    c.itemFx.bc = itemfx::decodeIronBall(w.bc);
    itemfx::decodeLevelSfx(w.ls, c.itemFx.ls);
    c.itemFx.hx = itemfx::decodeHeldExtra(w.hx);
    c.itemFx.fr = fishing::decode(w.fr);
    c.horse = horse::decode(w);
    c.hasPlayerUpdate = true;

    RemotePoseSample sample;
    sample.seq = packet.value("seq", 0u);
    sample.epoch = static_cast<uint8_t>(packet.value("ep", 0u));
    // Each dummy plays these when its playout reaches this packet (DummyItemFx).
    const auto xe = packet.find("xe");
    if (xe != packet.end() && xe->is_array()) {
        int taken = 0;
        for (const auto& entry : *xe) {
            if (taken == itemfx::kMaxEventsPerPacket) {
                break;
            }
            int32_t values[7];
            bool ok = entry.is_array() && entry.size() == 7;
            for (size_t k = 0; ok && k < 7; k++) {
                ok = entry[k].is_number_integer();
                values[k] = ok ? static_cast<int32_t>(entry[k].get<int64_t>()) : 0;
            }
            ItemFxEvent ev;
            if (ok && itemfx::decodeEvent(values, ev)) {
                c.itemFxEvents.push(sample.seq, ev);
                taken++;
            }
        }
    }
    traceTransformFx(c, prevTf, prevClip, prevSwap, prevForm, sample.seq);
    sample.state = makeLinkPuppetState(c);
    c.pose.push(sample, std::chrono::duration<double>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace presence {

void captureAndSend(daAlink_c* link) {
    const auto& pos = link->current.pos;
    const auto& ang = link->current.angle;
    const auto& shapeAng = link->shape_angle;
    uint16_t upperANMs[3];
    uint16_t lowerANMs[3];
    float upperFrames[3];
    float lowerFrames[3];
    float upperRatios[3];
    float lowerRatios[3];
    // loadModelDVD has freed the old body's model data while the swap runs: no clips then.
    const bool modelSwap = link->getClothesChangeWaitTimer() != 0;
    for (int i = 0; i < 3; i++) {
        upperANMs[i] = modelSwap ? 0 : Session::playerPackAnmId(*link, true, i);
        lowerANMs[i] = modelSwap ? 0 : Session::playerPackAnmId(*link, false, i);
        upperFrames[i] = link->mUpperFrameCtrl[i].getFrame();
        lowerFrames[i] = link->mUnderFrameCtrl[i].getFrame();
        upperRatios[i] = link->mNowAnmPackUpper[i].getRatio();
        lowerRatios[i] = link->mNowAnmPackUnder[i].getRatio();
    }
    int16_t waterDropColors[2][4] = {};
    int16_t swordUpColors[2][4] = {};
    for (int i = 0; i < 2; i++) {
        waterDropColors[i][0] = link->field_0x32a0[i].r;
        waterDropColors[i][1] = link->field_0x32a0[i].g;
        waterDropColors[i][2] = link->field_0x32a0[i].b;
        waterDropColors[i][3] = link->field_0x32a0[i].a;
        swordUpColors[i][0] = link->field_0x32b0[i].r;
        swordUpColors[i][1] = link->field_0x32b0[i].g;
        swordUpColors[i][2] = link->field_0x32b0[i].b;
        swordUpColors[i][3] = link->field_0x32b0[i].a;
    }
    // The body actually drawn, not the save's transform status.
    const int8_t tfStatus = link->checkWolf() ? 1 : 0;
    const uint8_t cutType = link->getCutType();
    const int blurAlpha = std::clamp<int>(link->m_swordBlur.field_0x20, 0, 0xFF);
    const bool swordBlurActive =
        link->mEquipItem == 0x0103 &&
        (link->getCutAtFlg() || link->m_swordBlur.field_0x14 > 0 ||
            (cutType != daPy_py_c::CUT_TYPE_NONE && !link->checkCutTypeNoBlur()));
    const bool attentionLock = link->checkAttentionLock() != FALSE;
    const uint8_t shieldItem = dComIfGs_getSelectEquipShield();
    const bool shieldInHand =
        shieldItem != dItemNo_NONE_e &&
        (attentionLock || link->checkPlayerGuardAndAttack() || link->checkUpperGuardAnime() ||
            link->checkSmallUpperGuardAnime() || link->mEquipItem == 0x0103);
    const bool swordChargeActive = link->mEquipItem == 0x0103 && link->checkCutCharge() != FALSE;
    const float swordChargeFrame = link->m_nSwordBtk ? link->m_nSwordBtk->getFrame() : 0.0f;
    uint8_t itemProjectileType = 0;
    if (link->mEquipItem == dItemNo_PACHINKO_e) {
        itemProjectileType = 3;
    } else if (daPy_py_c::checkBowItem(link->mEquipItem)) {
        itemProjectileType =
            (link->mEquipItem == dItemNo_BOMB_ARROW_e || link->field_0x301e == 1) ? 2 : 1;
    }

    static uint16_t sItemProjectileSeq = 0;
    static bool sProjectileShootWasActive = false;
    const bool projectileShootActive = itemProjectileType != 0 && link->checkBowShootAnime();
    if (projectileShootActive && !sProjectileShootWasActive) {
        sItemProjectileSeq++;
    }
    sProjectileShootWasActive = projectileShootActive;

    fopAc_ac_c* itemActor = link->mItemAcKeep.getActor();
    const bool arrowLoaded = itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_ARROW_e &&
                             fopAcM_GetParam(itemActor) == 0;
    const bool slingLoaded =
        link->mEquipItem == dItemNo_PACHINKO_e && link->checkBowReadyAnime();
    const bool itemAmmoLoaded = arrowLoaded || slingLoaded;

    uint16_t visFlags = 0;
    if (link->checkEquipHeavyBoots()) {
        visFlags |= kVisHeavyBoots;
    }
    if (link->checkZoraWearMaskDraw()) {
        visFlags |= kVisZoraMask;
    }
    if (link->checkNoResetFlg0(daPy_py_c::FLG0_PLAYER_NO_DRAW)) {
        visFlags |= kVisNoDraw;
    }
    // In hand the lantern goes with the equip item.
    if (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) && link->mEquipItem != dItemNo_KANTERA_e &&
        !link->checkOilBottleItemNotGet(link->mEquipItem))
    {
        visFlags |= kVisLanternBelt;
    }
    if (link->mEquipItem == dItemNo_COPY_ROD_e && link->checkCopyRodTopUse()) {
        visFlags |= kVisCopyRodLit;
    }
    visFlags |= pvp::localVisFlags(link);

    Session::instance().sendPlayerUpdate(pos.x, pos.y, pos.z, ang.x, ang.y, ang.z, shapeAng.x,
        shapeAng.y, shapeAng.z, link->mBodyAngle.x, link->mBodyAngle.y, link->mBodyAngle.z,
        link->field_0x30c8, attentionLock, shieldInHand, tfStatus, modelSwap, upperANMs,
        lowerANMs, upperFrames, lowerFrames, upperRatios, lowerRatios, waterDropColors,
        swordUpColors, dComIfGs_getSelectEquipSword(), shieldItem,
        dComIfGs_getSelectEquipClothes(), static_cast<uint16_t>(link->mEquipItem),
        link->field_0x2fb6, link->field_0x3444, cutType, swordBlurActive,
        static_cast<uint8_t>(blurAlpha), link->mLeftHandIndex, link->mRightHandIndex,
        link->field_0x2f94, link->field_0x2f95, link->field_0x2f96, link->field_0x2f97,
        link->mLeftItemJntNo, link->mRightItemJntNo, link->mAnmHeap9.getIdx(),
        link->field_0x33dc, itemAmmoLoaded, sItemProjectileSeq, itemProjectileType,
        swordChargeActive, swordChargeFrame, visFlags);
}

}  // namespace presence

}  // namespace twili
