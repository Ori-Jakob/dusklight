#include "fx/ItemFx.hpp"

#include "core/Session.hpp"
#include "pvp/Pvp.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_boomerang.h"
#include "d/actor/d_a_crod.h"
#include "d/actor/d_a_nbomb.h"
#include "d/actor/d_a_spinner.h"
#include "d/d_cc_d.h"
#include "d/d_com_inf_game.h"
#include "d/d_particle_name.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace twili::itemfx {
namespace {

constexpr uint32_t kMaxEventAgeTicks = 4;
constexpr int kMaxCandidates = 24;
constexpr int kMaxPending = 16;

struct Tracked {
    fpc_ProcID actor = fpcM_ERROR_PROCESS_ID_e;
    int candidate = -1;
    ItemFxSlot slot;       // what the last capture put in this slot
    cXyz actorPos;         // a bomb's current.pos at the last capture (bombPose)
    bool actorPosValid = false;
};

struct Candidate {
    fopAc_ac_c* actor;
    uint8_t kind;
    uint8_t rank;
};

struct Scan {
    Candidate c[kMaxCandidates];
    int n = 0;
};

struct Pending {
    ItemFxEvent ev;
    uint32_t seq;
};

// A level sound noted at pose tick `seq` (localPoseSeq)
struct LevelNote {
    RemoteLevelSfx sfx;
    uint32_t seq;
};

struct Sender {
    Tracked slots[kItemFxSlots];
    uint16_t nextId = 1;
    Pending pending[kMaxPending];
    int pendingCount = 0;
    LevelNote level[kItemFxLevelSfx];
    int levelCount = 0;
    RemoteItemFx last;
    SenderStats stats;
    RemoteItemFx injected;  // injectForTest
    int injectTicks = 0;
};

Sender s_tx;

uint16_t clampAux(float v) {
    return std::isfinite(v) ? static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f)) : 0;
}

void queueEvent(uint8_t type, uint32_t arg, const cXyz& pos, int16_t rotX, int16_t rotY,
                uint32_t seq) {
    ItemFxEvent ev;
    ev.type = type;
    ev.arg = arg;
    ev.pos[0] = pos.x;
    ev.pos[1] = pos.y;
    ev.pos[2] = pos.z;
    ev.rot[0] = rotX;
    ev.rot[1] = rotY;
    if (s_tx.pendingCount == kMaxPending) {
        std::copy(s_tx.pending + 1, s_tx.pending + kMaxPending, s_tx.pending);
        s_tx.pendingCount--;
    }
    s_tx.pending[s_tx.pendingCount++] = {ev, seq};
}

cXyz slotPos(const ItemFxSlot& s) {
    return cXyz(s.pos[0], s.pos[1], s.pos[2]);
}

// One actor list pass
void* scanJudge(void* p, void* data) {
    fopAc_ac_c* actor = static_cast<fopAc_ac_c*>(p);
    Scan* scan = static_cast<Scan*>(data);
    if (scan->n >= kMaxCandidates || !fopAcM_IsActor(actor)) {
        return nullptr;
    }
    const u32 param = fopAcM_GetParam(actor);
    daAlink_c* link = daAlink_getAlinkActorClass();
    switch (fopAcM_GetName(actor)) {
    case fpcNm_SPINNER_e:
        // daSpinner_c::draw's condition.
        if (!static_cast<daSpinner_c*>(actor)->getDeleteFlg() &&
            !(link->checkSpinnerReady() && link->gravity >= 0.0f))
        {
            scan->c[scan->n++] = {actor, kItemFxSpinner, 1};
        }
        break;
    case fpcNm_CROD_e:
        // daCrod_c::draw's.
        if (param != 6 && (link->checkCopyRodTopUse() || link->checkCopyRodRevive())) {
            scan->c[scan->n++] = {actor, kItemFxCrodBall, 2};
        }
        break;
    case fpcNm_ARROW_e: {
        daArrow_c* arrow = static_cast<daArrow_c*>(actor);
        if (param != 0 && arrow->mArrowType != daArrow_c::ARROW_TYPE_LIGHT &&
            !(arrow->field_0x93f != 0))
        {
            scan->c[scan->n++] = {actor, kItemFxArrow, uint8_t(param == 1 || param == 2 ? 2 : 3)};
        }
        break;
    }
    case fpcNm_BOOMERANG_e:
        if (param != 0) {
            scan->c[scan->n++] = {actor, kItemFxBoomerang, 0};
        }
        break;
    case fpcNm_NBOMB_e: {
        daNbomb_c* bomb = static_cast<daNbomb_c*>(actor);
        if (bomb->checkPlayerMake() && param != dBomb_c::PRM_NORMAL_BOMB_EXPLODE) {
            scan->c[scan->n++] = {actor, kItemFxBomb, 1};
        }
        break;
    }
    default:
        break;
    }
    return nullptr;
}

void fillArrow(daArrow_c* arrow, uint32_t seq, ItemFxSlot& s) {
    const u32 param = fopAcM_GetParam(arrow);
    s.sub = arrow->mArrowType;
    switch (param) {
    case 3:
        s.state = kItemFxArrowStuckActor;
        break;
    case 4:
        s.state = kItemFxArrowStuckBg;
        break;
    case 5:
        s.state = kItemFxArrowRebound;
        break;
    case 6:
        s.state = kItemFxArrowHeld;
        break;
    case 8:
        s.state = kItemFxArrowSlingHit;
        break;
    default:
        s.state = kItemFxArrowFly;
        break;
    }
    if (param == 2) {
        s.flags |= kItemFxArrowCharge;
    }
    if ((arrow->field_0x943 != 0)) {
        s.flags |= kItemFxArrowFrozen;
    }
    if ((arrow->field_0x945 != 0)) {
        s.flags |= kItemFxArrowUnderwater;
    }
    s.pos[0] = arrow->current.pos.x;
    s.pos[1] = arrow->current.pos.y;
    s.pos[2] = arrow->current.pos.z;
    s.ang[0] = arrow->shape_angle.x;
    s.ang[1] = arrow->shape_angle.y;
    s.ang[2] = arrow->shape_angle.z;
    // The bomb arrow's fuse (draw's flash, execute's explosion) as the tick it ends on
    s.aux = s.sub == kItemFxArrowBomb
                ? static_cast<uint16_t>(seq + arrow->field_0x950)
                : clampAux(arrow->scale.x * 256.0f);
}

void fillBoomerang(daBoomerang_c* boom, ItemFxSlot& s) {
    s.state = boom->getReturnFlg() ? kItemFxBoomReturn : kItemFxBoomOut;
    s.pos[0] = boom->current.pos.x;
    s.pos[1] = boom->current.pos.y;
    s.pos[2] = boom->current.pos.z;
    s.ang[0] = boom->shape_angle.x;
    s.ang[1] = boom->shape_angle.y;
    s.ang[2] = boom->shape_angle.z;
    s.aux = clampAux(boom->m_shippuSize * 256.0f);
}

// The bomb's pose is its model matrix
void fillBomb(daNbomb_c* bomb, Tracked& t, uint32_t seq, ItemFxSlot& s) {
    s.sub = bomb->checkStateFlg0(daNbomb_c::FLG0_INSECT_BOMB) ? kItemFxBombInsect
            : bomb->checkWaterBomb()                          ? kItemFxBombWater
                                                              : kItemFxBombNormal;
    const bool carried = fopAcM_checkCarryNow(bomb) || fopAcM_checkHookCarryNow(bomb);
    s.state = bomb->checkStateFlg0(daNbomb_c::FLG0_UNK_800) ? kItemFxBombSinking
              : carried                                      ? kItemFxBombCarried
                                                             : kItemFxBombFree;
    if (bomb->checkStateFlg0(daNbomb_c::FLG0_UNDERWATER)) {
        s.flags |= kItemFxBombUnderwater;
    }
    if (bomb->checkStateFlg0(daNbomb_c::FLG0_FROZEN)) {
        s.flags |= kItemFxBombFrozen;
    }
    const bool timerStop = bomb->checkTimerStop();
    if (timerStop) {
        s.flags |= kItemFxBombTimerStop;
    }
    MtxP mtx = bomb->mpModel->getBaseTRMtx();
    cXyz pos(mtx[0][3], mtx[1][3], mtx[2][3]);
    if (carried && t.actorPosValid) {
        pos += bomb->current.pos - t.actorPos;
    }
    t.actorPos = bomb->current.pos;
    t.actorPosValid = true;
    csXyz rot;
    mDoMtx_MtxToRot(mtx, &rot);
    s.pos[0] = pos.x;
    s.pos[1] = pos.y;
    s.pos[2] = pos.z;
    s.ang[0] = rot.x;
    s.ang[1] = rot.y;
    s.ang[2] = rot.z;
    s.aux = timerStop ? clampAux(bomb->scale.x * 256.0f)
                      : static_cast<uint16_t>(seq + bomb->getExTime());
}

// The spinner's model matrix, like a bomb's
void fillSpinner(daSpinner_c* spinner, daAlink_c* link, ItemFxSlot& s) {
    s.state = kItemFxSpinnerOut;
    if (spinner->checkPathMoveNow() != nullptr) {
        s.flags |= kItemFxSpinnerRail;
    }
    if (spinner->reflectAccept()) {
        s.flags |= kItemFxSpinnerSparks;
    }
    if ((spinner->field_0xa76 < 0)) {
        s.flags |= kItemFxSpinnerReverse;
    }
    if (link->checkSpinnerRideOwn(spinner)) {
        s.flags |= kItemFxSpinnerRidden;
    }
    if (spinner->checkSpinnerTagInto()) {
        s.flags |= kItemFxSpinnerTagInto;
    }
    MtxP mtx = spinner->getModelMtx();
    csXyz rot;
    mDoMtx_MtxToRot(mtx, &rot);
    s.pos[0] = mtx[0][3];
    s.pos[1] = mtx[1][3];
    s.pos[2] = mtx[2][3];
    s.ang[0] = rot.x;
    s.ang[1] = rot.y;
    s.ang[2] = rot.z;
    s.aux = clampAux(spinner->mBck.getFrame() * kFrameScale);
}

void fillCrod(daCrod_c* crod, ItemFxSlot& s) {
    const u32 param = fopAcM_GetParam(crod);
    s.state = param <= 1   ? kItemFxCrodAtRod
              : param <= 3 ? kItemFxCrodFly
              : param == 4 ? kItemFxCrodStatue
                           : kItemFxCrodReturn;
    if (param == 1) {
        s.flags |= kItemFxCrodAim;
    }
    if (s.state != kItemFxCrodAtRod) {
        s.pos[0] = crod->current.pos.x;
        s.pos[1] = crod->current.pos.y;
        s.pos[2] = crod->current.pos.z;
    }
}

// One-shots the real actor played on this transition, at the pose it made them.
void noteTransition(const ItemFxSlot& prev, const ItemFxSlot& now, uint32_t seq) {
    // A new object (no state yet) made none
    if (prev.kind != now.kind || prev.id != now.id || prev.state == 0) {
        return;
    }
    const cXyz pos = slotPos(now);
    if (now.kind == kItemFxArrow) {
        if (prev.state == kItemFxArrowFly && now.state != kItemFxArrowFly) {
            switch (now.state) {
            case kItemFxArrowStuckBg:  // procMove's stick
                queueEvent(kItemFxEvSound, Z2SE_HIT_AL_ARROW_STICK, pos, 0, 0, seq);
                break;
            case kItemFxArrowRebound:  // procMove's rebound off hard ground
                queueEvent(kItemFxEvHitMark, 9, pos, prev.ang[0], prev.ang[1], seq);
                queueEvent(kItemFxEvSound, Z2SE_HIT_AL_ARROW_REBOUND, pos, 0, 0, seq);
                break;
            case kItemFxArrowSlingHit:  // procSlingHitInit
                queueEvent(kItemFxEvHitMark, 9, pos, prev.ang[0], prev.ang[1], seq);
                queueEvent(kItemFxEvSound, Z2SE_HIT_PACHINKO, pos, 0, 0, seq);
                break;
            default:
                break;
            }
        }
        if (!(prev.flags & kItemFxArrowUnderwater) && (now.flags & kItemFxArrowUnderwater)) {
            queueEvent(kItemFxEvWater, 5, pos, 0, 0, seq);  // setArrowWaterNextPos's 0.3
        }
    } else if (now.kind == kItemFxBomb) {
        const bool wasIn = (prev.flags & kItemFxBombUnderwater) || prev.state == kItemFxBombSinking;
        const bool isIn = (now.flags & kItemFxBombUnderwater) || now.state == kItemFxBombSinking;
        if (!wasIn && isIn) {
            queueEvent(kItemFxEvWater, 16, pos, 0, 0, seq);  // execute's pillar, scale 1
        }
    } else if (now.kind == kItemFxSpinner) {
        const daAlink_c* link = daAlink_getAlinkActorClass();
        if (!(prev.flags & kItemFxSpinnerTagInto) && (now.flags & kItemFxSpinnerTagInto) &&
            link != nullptr)
        {
            // execute's spark as it drops into the slot, at our feet.
            queueEvent(kItemFxEvParticle, 0xE7, link->current.pos, 0, 0, seq);
        }
    }
}

void captureHookshot(daAlink_c* link, RemoteHookshot& h) {
    h = RemoteHookshot{};
    if (!daPy_py_c::checkHookshotItem(link->mEquipItem) || link->mHeldItemModel == nullptr ||
        link->mpHookTipModel == nullptr)
    {
        return;
    }
    int mode = link->mItemMode;
    if (mode == 2) {
        mode = kItemFxHookReady;  // the tick between release and shot
    } else if (mode < 0 || mode > kItemFxHookReturn) {
        mode = kItemFxHookNone;
    }
    // setHookshotPos's cases for the other tip.
    h.sub = dComIfGp_checkPlayerStatus1(0, 0x10000)     ? kItemFxHookSubRoof
            : dComIfGp_checkPlayerStatus1(0, 0x2000000) ? kItemFxHookSubWall
            : link->field_0x3024 != 0                   ? kItemFxHookSubReturn
                                                        : kItemFxHookSubHome;
    h.mode = static_cast<uint8_t>(mode);
    if (!h.active()) {
        return;
    }
    h.hand = link->field_0x3020 != 0 ? 1 : 0;
    h.stopTime = static_cast<uint8_t>(std::clamp<int>(link->field_0x3026, 0, 15));
    h.tipFrame = link->field_0x33e0;
    if (h.out()) {
        h.tip[0] = link->mHookshotTopPos.x;
        h.tip[1] = link->mHookshotTopPos.y;
        h.tip[2] = link->mHookshotTopPos.z;
        h.tipAng[0] = link->field_0x301c;
        h.tipAng[1] = link->field_0x301e;
        s_tx.stats.hookOutTicks++;
        s_tx.stats.hookMaxDist = std::max(s_tx.stats.hookMaxDist,
                                          link->mHookshotTopPos.abs(link->mHeldItemRootPos));
    }
    if (h.sub != kItemFxHookSubHome) {
        h.subTip[0] = link->mIronBallBgChkPos.x;
        h.subTip[1] = link->mIronBallBgChkPos.y;
        h.subTip[2] = link->mIronBallBgChkPos.z;
        h.subAng = link->field_0x3022;
    }
}

void captureIronBall(daAlink_c* link, RemoteIronBall& b) {
    b = RemoteIronBall{};
    if (link->mEquipItem != dItemNo_IRONBALL_e || link->mIronBallChainPos == nullptr ||
        link->mIronBallChainAngle == nullptr)
    {
        return;
    }
    const int mode = link->mItemVar0.field_0x3018;
    if (mode <= 0 || mode > 8) {
        // setIronBallPos's mode 0
        b.aim = link->mProcID == daAlink_c::PROC_IRON_BALL_SUBJECT ||
                link->mProcID == daAlink_c::PROC_IRON_BALL_MOVE;
        return;
    }
    b.mode = static_cast<uint8_t>(mode);
    b.links = static_cast<int16_t>(std::clamp<int>(link->mItemMode, 0, 100));
    b.ball[0] = link->mIronBallChainPos[0].x;
    b.ball[1] = link->mIronBallChainPos[0].y;
    b.ball[2] = link->mIronBallChainPos[0].z;
    b.ballAng[0] = link->mIronBallChainAngle[0].x;
    b.ballAng[1] = link->mIronBallChainAngle[0].y;
    b.ballAng[2] = link->mIronBallChainAngle[0].z;
    s_tx.stats.ironBallTicks++;
}

// The texture clip the bottle's liquid follows
uint8_t currentBottleBtk(daAlink_c* link, float& frame) {
    J3DAnmTextureSRTKey* const btks[3] = {link->field_0x0718, link->field_0x071c,
                                          link->field_0x0720};
    J3DModelData* data = link->mHeldItemModel->getModelData();
    for (u16 m = 0; m < data->getMaterialNum(); m++) {
        J3DMaterialAnm* anm = data->getMaterialNodePointer(m)->getMaterialAnm();
        if (anm == nullptr) {
            continue;
        }
        for (int t = 0; t < 8; t++) {
            J3DAnmTextureSRTKey* cur = anm->getTexMtxAnm(t).getAnimation();
            for (int i = 0; cur != nullptr && i < 3; i++) {
                if (btks[i] == cur) {
                    frame = cur->getFrame();
                    return static_cast<uint8_t>(i + 1);
                }
            }
        }
    }
    return 0;
}

void captureHeldExtra(daAlink_c* link, RemoteHeldExtra& x) {
    x = RemoteHeldExtra{};
    if (link->mEquipItem == 0x104 && (link->mProcID == daAlink_c::PROC_GRASS_WHISTLE_GET ||
                                      link->mProcID == daAlink_c::PROC_GRASS_WHISTLE_WAIT))
    {
        // procGrassWhistleGet's setGrassWhistleModel(mProcVar2.field_0x300c).
        x.grassType = link->mProcVar2.field_0x300c == 1 ? 2 : 1;
    } else if (daPy_py_c::checkBottleItem(link->mEquipItem) && link->mHeldItemModel != nullptr) {
        x.bottleBtk = currentBottleBtk(link, x.bottleBtkFrame);
        if (link->field_0x072c != nullptr) {
            x.bottleBtpFrame = link->field_0x072c->getFrame();
        }
    }
}

// This tick's level sounds
void captureLevelSfx(uint32_t seq, RemoteLevelSfx (&out)[kItemFxLevelSfx]) {
    for (RemoteLevelSfx& l : out) {
        l = RemoteLevelSfx{};
    }
    int n = 0;
    for (int i = 0; i < s_tx.levelCount; i++) {
        if (s_tx.level[i].seq == seq - 1) {
            out[n++] = s_tx.level[i].sfx;
        }
    }
    if (n > 0) {
        s_tx.stats.levelSfxTicks++;
    }
}

// The object of slot `t` is gone from our candidates.
void endTracked(const Tracked& t, uint32_t seq) {
    fopAc_ac_c* actor = fopAcM_SearchByID(t.actor);
    if (actor == nullptr || t.slot.kind != kItemFxBomb || fopAcM_GetName(actor) != fpcNm_NBOMB_e ||
        fopAcM_GetParam(actor) != dBomb_c::PRM_NORMAL_BOMB_EXPLODE)
    {
        return;
    }
    daNbomb_c* bomb = static_cast<daNbomb_c*>(actor);
    const bool underwater = bomb->checkStateFlg0(daNbomb_c::FLG0_UNDERWATER) != 0;
    queueEvent(kItemFxEvExplode, underwater ? 1 : 0, bomb->current.pos, 0, bomb->shape_angle.y,
               seq);
}

}  // namespace

void noteBombArrowExplode(const cXyz& pos, bool underwater) {
    pvp::noteLocalExplosion(pos);
    if (Session::instance().isConnected()) {
        // procExplodeInit's random yaw for the particles.
        queueEvent(kItemFxEvExplode, underwater ? 1 : 0, pos, 0,
                   static_cast<int16_t>(cM_rndF(65536.0f)), Session::localPoseSeq());
    }
}

void noteLevelSfx(uint32_t id, uint8_t kind, uint32_t mapInfo) {
    if (!Session::instance().isConnected()) {
        return;
    }
    const uint32_t seq = Session::localPoseSeq();
    if (s_tx.levelCount > 0 && s_tx.level[0].seq != seq) {
        s_tx.levelCount = 0;
    }
    for (int i = 0; i < s_tx.levelCount; i++) {
        if (s_tx.level[i].sfx.id == id && s_tx.level[i].sfx.kind == kind) {
            return;
        }
    }
    if (s_tx.levelCount == kItemFxLevelSfx) {
        s_tx.stats.levelSfxDropped++;
        return;
    }
    LevelNote& n = s_tx.level[s_tx.levelCount++];
    n.sfx.kind = kind;
    n.sfx.mapInfo = static_cast<uint8_t>(std::min<uint32_t>(mapInfo, 0xFF));
    n.sfx.id = id;
    n.seq = seq;
}

void noteParticle(uint16_t id, int count, const cXyz& pos, int16_t rotX, int16_t rotY,
                  float scale) {
    if (!Session::instance().isConnected() || count < 1) {
        return;
    }
    const uint32_t scale16 =
        scale == 1.0f ? 0 : static_cast<uint32_t>(std::clamp(scale * 16.0f, 1.0f, 255.0f));
    queueEvent(kItemFxEvParticle,
               id | (static_cast<uint32_t>(std::min(count, 16) - 1) << 16) | (scale16 << 20), pos,
               rotX, rotY, Session::localPoseSeq());
}

void noteHitMark(uint16_t type, const cXyz& pos, int16_t rotX, int16_t rotY) {
    if (Session::instance().isConnected()) {
        queueEvent(kItemFxEvHitMark, type, pos, rotX, rotY, Session::localPoseSeq());
    }
}

void noteSound(uint32_t id, const cXyz& pos, uint32_t mapInfo) {
    if (Session::instance().isConnected()) {
        queueEvent(kItemFxEvSound, id, pos, static_cast<int16_t>(std::min<uint32_t>(mapInfo, 53)),
                   0, Session::localPoseSeq());
    }
}

void noteWater(const cXyz& pos, float scale) {
    if (Session::instance().isConnected()) {
        queueEvent(kItemFxEvWater,
                   static_cast<uint32_t>(std::clamp(scale * 16.0f + 0.5f, 1.0f, 64.0f)), pos, 0,
                   0, Session::localPoseSeq());
    }
}

int captureLocal(uint32_t seq, RemoteItemFx& out, ItemFxEvent events[kMaxEventsPerPacket]) {
    out = RemoteItemFx{};
    Scan scan;
    if (daAlink_getAlinkActorClass() != nullptr) {
        fopAcM_Search(scanJudge, &scan);
    }

    bool claimed[kMaxCandidates] = {};
    for (Tracked& t : s_tx.slots) {
        t.candidate = -1;
        if (t.actor == fpcM_ERROR_PROCESS_ID_e) {
            continue;
        }
        for (int i = 0; i < scan.n; i++) {
            if (!claimed[i] && fopAcM_GetID(scan.c[i].actor) == t.actor &&
                scan.c[i].kind == t.slot.kind)
            {
                t.candidate = i;
                claimed[i] = true;
                break;
            }
        }
        if (t.candidate < 0) {
            endTracked(t, seq);
            t = Tracked{};
        }
    }

    // New objects, most important first
    int order[kMaxCandidates];
    for (int i = 0; i < scan.n; i++) {
        order[i] = i;
    }
    std::stable_sort(order, order + scan.n,
                     [&](int a, int b) { return scan.c[a].rank < scan.c[b].rank; });
    for (int k = 0; k < scan.n; k++) {
        const int i = order[k];
        if (claimed[i]) {
            continue;
        }
        int slot = -1;
        for (int j = 0; j < kItemFxSlots && slot < 0; j++) {
            if (s_tx.slots[j].actor == fpcM_ERROR_PROCESS_ID_e) {
                slot = j;
            }
        }
        if (slot < 0 && scan.c[i].rank <= 2) {
            for (int j = 0; j < kItemFxSlots && slot < 0; j++) {
                const ItemFxSlot& s = s_tx.slots[j].slot;
                if (s.kind == kItemFxArrow && s.state != kItemFxArrowFly) {
                    slot = j;
                    s_tx.stats.evicted++;
                }
            }
        }
        if (slot < 0) {
            continue;
        }
        Tracked& t = s_tx.slots[slot];
        t = Tracked{};
        t.actor = fopAcM_GetID(scan.c[i].actor);
        t.candidate = i;
        t.slot.kind = scan.c[i].kind;
        t.slot.id = s_tx.nextId;
        s_tx.nextId = s_tx.nextId == 0xFFFF ? 1 : s_tx.nextId + 1;
        claimed[i] = true;
        s_tx.stats.objects[t.slot.kind]++;
    }

    for (int j = 0; j < kItemFxSlots; j++) {
        Tracked& t = s_tx.slots[j];
        if (t.candidate < 0) {
            continue;
        }
        fopAc_ac_c* actor = scan.c[t.candidate].actor;
        ItemFxSlot now;
        now.kind = t.slot.kind;
        now.id = t.slot.id;
        switch (now.kind) {
        case kItemFxArrow:
            fillArrow(static_cast<daArrow_c*>(actor), seq, now);
            break;
        case kItemFxBoomerang:
            fillBoomerang(static_cast<daBoomerang_c*>(actor), now);
            break;
        case kItemFxBomb:
            fillBomb(static_cast<daNbomb_c*>(actor), t, seq, now);
            break;
        case kItemFxSpinner:
            fillSpinner(static_cast<daSpinner_c*>(actor), daAlink_getAlinkActorClass(), now);
            break;
        case kItemFxCrodBall:
            fillCrod(static_cast<daCrod_c*>(actor), now);
            break;
        default:
            break;
        }
        noteTransition(t.slot, now, seq);
        t.slot = now;
        out.slots[j] = now;
    }
    if (daAlink_c* link = daAlink_getAlinkActorClass()) {
        captureHookshot(link, out.hk);
        captureIronBall(link, out.bc);
        captureHeldExtra(link, out.hx);
    }
    captureLevelSfx(seq, out.ls);
    if (s_tx.injectTicks > 0) {
        s_tx.injectTicks--;
        for (int j = 0; j < kItemFxSlots; j++) {
            if (s_tx.injected.slots[j].active()) {
                out.slots[j] = s_tx.injected.slots[j];
            }
        }
        if (s_tx.injected.hk.active()) {
            out.hk = s_tx.injected.hk;
        }
        if (s_tx.injected.bc.active()) {
            out.bc = s_tx.injected.bc;
        }
    }
    s_tx.last = out;

    // Events in the order they happened
    int count = 0;
    int kept = 0;
    for (int i = 0; i < s_tx.pendingCount; i++) {
        const Pending& p = s_tx.pending[i];
        if (seq - p.seq > kMaxEventAgeTicks) {
            continue;
        }
        if (count < kMaxEventsPerPacket) {
            events[count++] = p.ev;
            s_tx.stats.events[p.ev.type < kItemFxEvTypeCount ? p.ev.type : 0]++;
        } else {
            s_tx.pending[kept++] = p;
        }
    }
    s_tx.pendingCount = kept;
    return count;
}

uint16_t localVisFlags() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return 0;
    }
    uint16_t flags = 0;
    // daBoomerang_c::draw's condition for mp_setboomEfModel.
    if (link->mEquipItem == dItemNo_BOOMERANG_e && dComIfGp_checkPlayerStatus0(0, 0x80000)) {
        flags |= kVisBoomerangCharge;
    }
    if (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) &&
        link->checkNoResetFlg1(daPy_py_c::FLG1_UNK_80) &&
        !(link->mEquipItem == 0x103 && link->checkNoResetFlg3(daPy_py_c::FLG3_UNK_100000)))
    {
        flags |= kVisLanternLit;
        if (link->checkKandelaarSwingAnime() || link->mProcID == daAlink_c::PROC_KANDELAAR_SWING) {
            flags |= kVisLanternSwing;
        }
    }
    return flags;
}

void encodeSlot(const ItemFxSlot& s, int32_t x[9]) {
    if (!s.active()) {
        std::fill(x, x + 9, 0);
        return;
    }
    x[0] = s.kind | (s.sub << 4) | (s.state << 8) | (s.flags << 12);
    x[1] = s.id;
    for (int k = 0; k < 3; k++) {
        x[2 + k] = static_cast<int32_t>(std::clamp<long long>(
            std::isfinite(s.pos[k]) ? std::llround(s.pos[k] * kPosScale) : 0, -2000000000LL,
            2000000000LL));
        x[5 + k] = s.ang[k];
    }
    x[8] = s.aux;
}

ItemFxSlot decodeSlot(const int32_t x[9]) {
    ItemFxSlot s;
    const uint32_t head = static_cast<uint32_t>(x[0]);
    const uint8_t kind = head & 0xF;
    const uint8_t sub = (head >> 4) & 0xF;
    const uint8_t state = (head >> 8) & 0xF;
    if (kind == kItemFxNone || kind >= kItemFxKindCount || state == 0) {
        return s;
    }
    switch (kind) {
    case kItemFxArrow:
        if ((sub != kItemFxArrowNormal && sub != kItemFxArrowBomb && sub != kItemFxArrowSling) ||
            state > kItemFxArrowHeld)
        {
            return s;
        }
        break;
    case kItemFxBoomerang:
        if (state > kItemFxBoomReturn) {
            return s;
        }
        break;
    case kItemFxBomb:
        if (sub > kItemFxBombInsect || state > kItemFxBombSinking) {
            return s;
        }
        break;
    case kItemFxSpinner:
        if (state != kItemFxSpinnerOut) {
            return s;
        }
        break;
    case kItemFxCrodBall:
        if (state > kItemFxCrodReturn) {
            return s;
        }
        break;
    default:
        return s;
    }
    s.kind = kind;
    s.sub = sub;
    s.state = state;
    s.flags = static_cast<uint8_t>((head >> 12) & 0xFF);
    s.id = static_cast<uint16_t>(x[1]);
    for (int k = 0; k < 3; k++) {
        s.pos[k] = x[2 + k] / kPosScale;
        s.ang[k] = static_cast<int16_t>(x[5 + k]);
    }
    s.aux = static_cast<uint16_t>(x[8]);
    return s;
}

namespace {

int32_t wirePos(float v) {
    return static_cast<int32_t>(std::clamp<long long>(
        std::isfinite(v) ? std::llround(v * kPosScale) : 0, -2000000000LL, 2000000000LL));
}

int32_t wireFrame(float v) {
    return static_cast<int32_t>(std::isfinite(v) ? std::clamp(v * kFrameScale, 0.0f, 65535.0f)
                                                 : 0.0f);
}

// The impacts captureLocal and daAlink's hooks send
bool knownSound(uint32_t id) {
    if (id == Z2SE_HIT_AL_ARROW_REBOUND || id == Z2SE_HIT_ARROW_REBOUND ||
        id == Z2SE_HIT_HOOKSHOT_REBOUND)
    {
        return true;
    }
    for (u8 i = 1; i < 24; i++) {
        if (id == dCcD_GObjInf::getHitSeID(i, 0)) {
            return true;
        }
    }
    return false;
}

bool knownParticle(uint16_t id, int count) {
    switch (id) {
    case ID_ZI_J_COLHIT_HIBANA:
    case ID_ZI_J_COLHIT_KIKUZU:
    case ID_ZI_J_COLHIT_ICE:
    case dPa_RM(ID_ZI_S_DOWNSNOW_A):
    case dPa_RM(ID_ZI_S_DOWNSNOW_B):
    case dPa_RM(ID_ZI_S_DOWNSAND_A):
    case 0xE7:
        return count == 1;
    case ID_ZI_J_LK_DJGIRI_A:
        return count == 6;
    default:
        return false;
    }
}

}  // namespace

void encodeHookshot(const RemoteHookshot& h, int32_t out[11]) {
    std::fill(out, out + 11, 0);
    if (!h.active()) {
        return;
    }
    out[0] = h.mode | (h.hand << 4) | (h.sub << 5) | (h.stopTime << 8);
    for (int k = 0; k < 3; k++) {
        out[1 + k] = wirePos(h.tip[k]);
        out[7 + k] = wirePos(h.subTip[k]);
    }
    out[4] = h.tipAng[0];
    out[5] = h.tipAng[1];
    out[6] = wireFrame(h.tipFrame);
    out[10] = h.subAng;
}

RemoteHookshot decodeHookshot(const int32_t in[11]) {
    RemoteHookshot h;
    const uint32_t head = static_cast<uint32_t>(in[0]);
    const uint8_t mode = head & 0xF;
    const uint8_t sub = (head >> 5) & 0x3;
    if (mode == 2 || mode > kItemFxHookReturn) {
        return h;
    }
    h.mode = mode;
    h.hand = (head >> 4) & 1;
    h.sub = sub;
    h.stopTime = (head >> 8) & 0xF;
    for (int k = 0; k < 3; k++) {
        h.tip[k] = in[1 + k] / kPosScale;
        h.subTip[k] = in[7 + k] / kPosScale;
    }
    h.tipAng[0] = static_cast<int16_t>(in[4]);
    h.tipAng[1] = static_cast<int16_t>(in[5]);
    // The open clip runs from 0 to 14 (setHookshotPos).
    h.tipFrame = std::clamp(in[6] / kFrameScale, 0.0f, 14.0f);
    h.subAng = static_cast<int16_t>(in[10]);
    return h;
}

void encodeIronBall(const RemoteIronBall& b, int32_t out[7]) {
    std::fill(out, out + 7, 0);
    if (!b.active()) {
        return;
    }
    out[0] = b.mode | ((b.aim ? 1 : 0) << 4) | (b.mode != 0 ? b.links << 8 : 0);
    if (b.mode != 0) {
        for (int k = 0; k < 3; k++) {
            out[1 + k] = wirePos(b.ball[k]);
            out[4 + k] = b.ballAng[k];
        }
    }
}

RemoteIronBall decodeIronBall(const int32_t in[7]) {
    RemoteIronBall b;
    const uint32_t head = static_cast<uint32_t>(in[0]);
    const uint8_t mode = head & 0xF;
    if (mode > 8) {
        return b;
    }
    b.aim = mode == 0 && ((head >> 4) & 1) != 0;
    if (mode == 0) {
        return b;
    }
    b.mode = mode;
    // The chain holds 102 points
    b.links = static_cast<int16_t>(std::min<uint32_t>((head >> 8) & 0xFF, 100));
    for (int k = 0; k < 3; k++) {
        b.ball[k] = in[1 + k] / kPosScale;
        b.ballAng[k] = static_cast<int16_t>(in[4 + k]);
    }
    return b;
}

void encodeLevelSfx(const RemoteLevelSfx (&ls)[kItemFxLevelSfx], int32_t out[8]) {
    for (int i = 0; i < kItemFxLevelSfx; i++) {
        out[2 * i] = ls[i].kind == 0 ? 0 : ls[i].kind | (ls[i].mapInfo << 8);
        out[2 * i + 1] = ls[i].kind == 0 ? 0 : static_cast<int32_t>(ls[i].id);
    }
}

void decodeLevelSfx(const int32_t in[8], RemoteLevelSfx (&ls)[kItemFxLevelSfx]) {
    for (int i = 0; i < kItemFxLevelSfx; i++) {
        ls[i] = RemoteLevelSfx{};
        const uint32_t head = static_cast<uint32_t>(in[2 * i]);
        const uint8_t kind = head & 0xFF;
        if (kind != static_cast<uint8_t>(PlayerSfxKind::VoiceLevel) &&
            kind != static_cast<uint8_t>(PlayerSfxKind::SoundLevel) &&
            kind != static_cast<uint8_t>(PlayerSfxKind::MapInfoLevel))
        {
            continue;
        }
        ls[i].kind = kind;
        ls[i].mapInfo = static_cast<uint8_t>(std::min<uint32_t>((head >> 8) & 0xFF, 53));
        ls[i].id = static_cast<uint32_t>(in[2 * i + 1]);
    }
}

void encodeHeldExtra(const RemoteHeldExtra& x, int32_t out[4]) {
    out[0] = x.grassType;
    out[1] = x.bottleBtk;
    out[2] = x.bottleBtk != 0 ? wireFrame(x.bottleBtkFrame) : 0;
    out[3] = wireFrame(x.bottleBtpFrame);
}

RemoteHeldExtra decodeHeldExtra(const int32_t in[4]) {
    RemoteHeldExtra x;
    x.grassType = in[0] == 1 || in[0] == 2 ? static_cast<uint8_t>(in[0]) : 0;
    if (in[1] >= 1 && in[1] <= 3) {
        x.bottleBtk = static_cast<uint8_t>(in[1]);
        x.bottleBtkFrame = in[2] / kFrameScale;
    }
    x.bottleBtpFrame = std::clamp(in[3] / kFrameScale, 0.0f, 16.0f);
    return x;
}

void encodeEvent(const ItemFxEvent& ev, int32_t out[7]) {
    out[0] = ev.type;
    out[1] = static_cast<int32_t>(ev.arg);
    for (int k = 0; k < 3; k++) {
        out[2 + k] = wirePos(ev.pos[k]);
    }
    out[5] = ev.rot[0];
    out[6] = ev.rot[1];
}

bool decodeEvent(const int32_t in[7], ItemFxEvent& out) {
    out = ItemFxEvent{};
    if (in[0] <= kItemFxEvNone || in[0] >= kItemFxEvTypeCount) {
        return false;
    }
    out.type = static_cast<uint8_t>(in[0]);
    out.arg = static_cast<uint32_t>(in[1]);
    // Hit marks index the game's own table
    if ((out.type == kItemFxEvHitMark && out.arg > 9) ||
        (out.type == kItemFxEvSound && !knownSound(out.arg)) ||
        (out.type == kItemFxEvWater && (out.arg == 0 || out.arg > 64)) ||
        (out.type == kItemFxEvExplode && out.arg > 1) ||
        (out.type == kItemFxEvParticle &&
         !knownParticle(out.arg & 0xFFFF, static_cast<int>((out.arg >> 16) & 0xF) + 1)))
    {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out.pos[k] = in[2 + k] / kPosScale;
    }
    out.rot[0] = static_cast<int16_t>(in[5]);
    out.rot[1] = static_cast<int16_t>(in[6]);
    return true;
}

const SenderStats& senderStats() {
    return s_tx.stats;
}

const RemoteItemFx& lastCaptured() {
    return s_tx.last;
}

void injectForTest(const RemoteItemFx& slots, int ticks, const ItemFxEvent* events, int count) {
    s_tx.injected = slots;
    s_tx.injectTicks = ticks;
    for (int i = 0; i < count; i++) {
        const ItemFxEvent& ev = events[i];
        queueEvent(ev.type, ev.arg, cXyz(ev.pos[0], ev.pos[1], ev.pos[2]), ev.rot[0], ev.rot[1],
                   Session::localPoseSeq());
    }
}

}  // namespace twili::itemfx

