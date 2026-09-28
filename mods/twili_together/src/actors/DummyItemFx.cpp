#include "actors/DummyItemFx.hpp"

#include "actors/DummyPlayer.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo_wether.h"
#include "d/d_particle.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_kankyo_mng.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2SeMgr.h"

#include <algorithm>
#include <cmath>

namespace twili {
namespace {

// Item sounds further than this from our player are not started (Z2 would not play them).
constexpr f32 kAudibleDistanceSq = 5000.0f * 5000.0f;
// A copy stuck in an actor stays this long
constexpr uint16_t kStuckInActorTicks = 30;
// The fuse sparks' particle callback reads the trace vector through a pointer
const cXyz kNoTrace(0.0f, 0.0f, 0.0f);

cXyz slotPos(const ItemFxSlot& s) {
    return cXyz(s.pos[0], s.pos[1], s.pos[2]);
}

// The pose tick, mod 2^16 like the fuse ends the sender sends.
uint16_t tick16(double shownSeq) {
    return static_cast<uint16_t>(static_cast<uint32_t>(std::max(0.0, std::floor(shownSeq))));
}

bool audible(const cXyz& pos) {
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    return player != nullptr && player->current.pos.abs2(pos) <= kAudibleDistanceSq;
}

// daArrow_c::setBlur for one trail emitter.
void setTrail(uint32_t& id, u16 name, const cXyz& pos, const dKy_tevstr_c& tev) {
    id = dComIfGp_particle_set(id, name, &pos, &tev);
    dComIfGp_particle_levelEmitterOnEventMove(id);
}

}  // namespace

void DummyItemFx::createHeap(const Models& m) {
    auto fill = [this](Pool pool, J3DModel* const* models, int count) {
        mPoolSize[pool] = 0;
        for (int i = 0; i < count && i < kMaxPoolSize; i++) {
            if (models[i] != nullptr) {
                mPools[pool][mPoolSize[pool]++] = models[i];
            }
        }
    };
    fill(kPoolArrow, m.arrow, kArrowPool);
    fill(kPoolBombArrow, m.bombArrow, kBombArrowPool);
    fill(kPoolSeed, m.seed, kSeedPool);
    fill(kPoolBoomerang, &m.boomerang, 1);
    fill(kPoolBomb, m.bomb, kBombPool);
    fill(kPoolWaterBomb, m.waterBomb, kBombPool);
    fill(kPoolInsect, m.insectBomb, kInsectPool);

    // daBoomerang_c::create
    mTornado = m.tornado;
    mTornadoBtk = m.tornadoBtk;
    mTornadoBckReady = false;
    if (mTornado != nullptr && m.tornadoBck != nullptr) {
        mTornadoBckReady = mTornadoBck.init(m.tornadoBck, 0, 2, 1.0f, 0, -1, false) != 0;
    }
    if (mTornado != nullptr) {
        if (mTornadoBtk != nullptr) {
            mTornadoBtk->searchUpdateMaterialID(mTornado->getModelData());
            mTornado->getModelData()->entryTexMtxAnimator(mTornadoBtk);
        }
        mTornado->getModelData()->getJointNodePointer(4)->setCallBack(tornadoJointCallBack);
        mTornado->setUserArea(reinterpret_cast<uintptr_t>(this));
    }
    mBoomGlow = m.boomGlow;
    mBoomGlowBtk = m.boomGlowBtk;
    if (mBoomGlow != nullptr && mBoomGlowBtk != nullptr) {
        mBoomGlowBtk->searchUpdateMaterialID(mBoomGlow->getModelData());
        mBoomGlow->getModelData()->entryTexMtxAnimator(mBoomGlowBtk);
    }
    // daNbomb_c::createHeap's clip for a bombling, plus the walk procInsectMoveInit switches to.
    mInsectBckReady = mPoolSize[kPoolInsect] != 0 && m.insectMoveBck != nullptr &&
                      m.insectWaitBck != nullptr &&
                      mInsectMoveBck.init(m.insectMoveBck, 0, 2, 1.0f, 0, -1, false) &&
                      mInsectWaitBck.init(m.insectWaitBck, 0, 2, 1.0f, 0, -1, false);
    // daSpinner_c::createHeap's spout clip
    fill(kPoolSpinner, &m.spinner, 1);
    mSpinnerBckReady = m.spinner != nullptr && m.spinnerBck != nullptr &&
                       mSpinnerBck.init(m.spinnerBck, 0, 0, 1.0f, 0, -1, false);
    fill(kPoolCrod, &m.crodBall, 1);
    mCrodBrk = m.crodBrk;
    mCrodBtk = m.crodBtk;
    if (m.crodBall != nullptr) {
        J3DModelData* data = m.crodBall->getModelData();
        if (mCrodBrk != nullptr) {
            mCrodBrk->searchUpdateMaterialID(data);
            data->entryTevRegAnimator(mCrodBrk);
        }
        if (mCrodBtk != nullptr) {
            mCrodBtk->searchUpdateMaterialID(data);
            data->entryTexMtxAnimator(mCrodBtk);
        }
    }
    mCrodBckReady = m.crodBall != nullptr && m.crodWaitBck != nullptr &&
                    m.crodAimBck != nullptr &&
                    mCrodWaitBck.init(m.crodWaitBck, 0, 2, 1.0f, 0, -1, false) &&
                    mCrodAimBck.init(m.crodAimBck, 0, 2, 1.0f, 0, -1, false);
}

void DummyItemFx::clearPointers() {
    for (int p = 0; p < kPoolCount; p++) {
        mPoolSize[p] = 0;
        for (int i = 0; i < kMaxPoolSize; i++) {
            mPools[p][i] = nullptr;
            mPoolUsed[p][i] = false;
        }
    }
    for (Visual& v : mVisual) {
        v.model = nullptr;
        v.pool = v.poolIndex = -1;
        v.slot = ItemFxSlot{};
    }
    mTornado = nullptr;
    mTornadoBtk = nullptr;
    mTornadoBckReady = false;
    mBoomGlow = nullptr;
    mBoomGlowBtk = nullptr;
    mInsectBckReady = false;
    mSpinnerBckReady = false;
    mCrodBrk = nullptr;
    mCrodBtk = nullptr;
    mCrodBckReady = false;
}

void DummyItemFx::init() {
    if (mSoundsReady) {
        return;
    }
    for (Visual& v : mVisual) {
        v.sound.init(&v.pos, 2);
        v.arrowSound.init(&v.pos, 1);
        v.groundY = -G_CM3D_F_INF;
    }
    mChargeSound.init(&mChargePos, 1);
    mSoundsReady = true;
}

// daBoomerang_c::windModelCallBack
int DummyItemFx::tornadoJointCallBack(J3DJoint*, int calcTiming) {
    if (calcTiming != 0) {
        return 1;
    }
    J3DModel* model = j3dSys.getModel();
    const DummyItemFx* fx = reinterpret_cast<const DummyItemFx*>(model->getUserArea());
    const int16_t* a = fx->mTornadoAng;
    mDoMtx_stack_c::YrotS(a[1]);
    mDoMtx_stack_c::ZrotM(-a[2]);
    mDoMtx_stack_c::XrotM(-a[0]);
    mDoMtx_stack_c::YrotM(-a[1]);
    mDoMtx_stack_c::concat(J3DSys::mCurrentMtx);
    mDoMtx_stack_c::get()[0][3] = J3DSys::mCurrentMtx[0][3];
    mDoMtx_stack_c::get()[1][3] = J3DSys::mCurrentMtx[1][3];
    mDoMtx_stack_c::get()[2][3] = J3DSys::mCurrentMtx[2][3];
    cMtx_copy(mDoMtx_stack_c::get(), J3DSys::mCurrentMtx);
    model->setAnmMtx(4, mDoMtx_stack_c::get());
    return 1;
}

DummyItemFx::Pool DummyItemFx::poolFor(const ItemFxSlot& s) const {
    switch (s.kind) {
    case kItemFxArrow:
        return s.sub == kItemFxArrowBomb   ? kPoolBombArrow
               : s.sub == kItemFxArrowSling ? kPoolSeed
                                            : kPoolArrow;
    case kItemFxBoomerang:
        return kPoolBoomerang;
    case kItemFxSpinner:
        return kPoolSpinner;
    case kItemFxCrodBall:
        return kPoolCrod;
    default:
        return s.sub == kItemFxBombWater    ? kPoolWaterBomb
               : s.sub == kItemFxBombInsect ? kPoolInsect
                                            : kPoolBomb;
    }
}

void DummyItemFx::acquire(daDummyPlayer_c& dummy, Visual& v, const ItemFxSlot& s) {
    v.slot = s;
    v.pool = static_cast<int8_t>(poolFor(s));
    v.poolIndex = -1;
    v.model = nullptr;
    for (int i = 0; i < mPoolSize[v.pool]; i++) {
        if (!mPoolUsed[v.pool][i]) {
            mPoolUsed[v.pool][i] = true;
            v.poolIndex = static_cast<int8_t>(i);
            v.model = mPools[v.pool][i];
            break;
        }
    }
    // A pooled model's last matrices were those of its previous object
    v.freshTicks = 1;
    v.stateTicks = 0;
    v.pos = slotPos(s);
    v.fxPos = v.fxPosPrev = v.pos;
    v.trace = cXyz::Zero;
    for (uint32_t& id : v.emitters) {
        id = 0;
    }
    v.trailAlpha = 0;
    v.spin = 0;
    v.frame = 0.0f;
    v.fuse = 0;
    v.scale = 1.0f;
    v.groundY = -G_CM3D_F_INF;
    v.shadowId = 0;
    dKy_tevstr_init(&v.tevStr, dummy.tevStr.room_no, 0xFF);
    mDebug.seen[s.kind]++;
}

void DummyItemFx::release(Visual& v) {
    if (v.slot.kind == kItemFxBomb) {
        if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[0])) {
            e->setUserWork(reinterpret_cast<uintptr_t>(&kNoTrace));
        }
    } else if (v.slot.kind == kItemFxCrodBall) {
        cutCrodLight();
    }
    for (uint32_t& id : v.emitters) {
        id = 0;
    }
    if (v.pool >= 0 && v.poolIndex >= 0) {
        mPoolUsed[v.pool][v.poolIndex] = false;
    }
    v.pool = v.poolIndex = -1;
    v.model = nullptr;
    v.placed = false;
    v.slot = ItemFxSlot{};
}

void DummyItemFx::update(daDummyPlayer_c& dummy, const LinkPuppetState& state, double shownSeq,
                         const ItemFxEventQueue& events, bool snapped, bool shown) {
    for (uint8_t& n : mDebug.drawn) {
        n = 0;
    }
    mDebug.emitters = 0;
    mDebug.tornado = false;
    mShown = shown;
    mTornadoPlaced = false;
    fireEvents(shownSeq, events, snapped, shown);
    if (!shown) {
        for (Explosion& e : mExplosions) {
            endExplosion(e);
        }
    }
    updateExplosions();

    const s8 reverb = dComIfGp_getReverb(dComIfGp_roomControl_getStayNo());
    for (int i = 0; i < kItemFxSlots; i++) {
        Visual& v = mVisual[i];
        const ItemFxSlot& s = state.itemFx.slots[i];
        const bool keep = shown && state.sendsItemFx && s.active();
        if (!keep || s.kind != v.slot.kind || s.id != v.slot.id) {
            if (v.slot.active()) {
                release(v);
            }
            if (!keep) {
                continue;
            }
            acquire(dummy, v, s);
        } else {
            if (v.freshTicks != 0) {
                v.freshTicks--;
            }
            v.stateTicks = s.state == v.slot.state ? v.stateTicks + 1 : 0;
        }
        const cXyz pos = slotPos(s);
        // The whole pose jumped, or this object did
        if (snapped || pos.abs2(v.pos) > kTeleportDistance * kTeleportDistance) {
            v.freshTicks = 1;
        }
        v.slot = s;
        v.pos = pos;
        v.placed = false;
        // Before any emitter
        g_env_light.settingTevStruct(0, &v.pos, &v.tevStr);
        switch (s.kind) {
        case kItemFxArrow:
            updateArrow(v, shownSeq, reverb);
            break;
        case kItemFxBoomerang:
            updateBoomerang(v, reverb);
            break;
        case kItemFxBomb:
            updateBomb(v, shownSeq, reverb);
            break;
        case kItemFxSpinner:
            updateSpinner(v);
            break;
        case kItemFxCrodBall:
            updateCrod(dummy, v);
            break;
        default:
            break;
        }
        if (v.placed && v.freshTicks == 0) {
            mDebug.drawn[s.kind]++;
        }
        for (uint32_t id : v.emitters) {
            if (id != 0 && dComIfGp_particle_getEmitter(id) != nullptr) {
                mDebug.emitters++;
            }
        }
    }
    if (mCrodLightOn) {
        mDebug.lights++;
    }
    updateCharge(dummy, state, shown);
    updateLevelSfx(dummy, state, shown);
    if (mSoundsReady) {
        for (Visual& v : mVisual) {
            v.sound.framework(0, reverb);
            v.arrowSound.framework(0, reverb);
        }
        mChargeSound.framework(0, reverb);
    }
}

void DummyItemFx::stopAll() {
    mShown = false;
    mTornadoPlaced = false;
    mBoomGlowPlaced = false;
    mDebug.boomCharge = false;
    for (Visual& v : mVisual) {
        if (v.slot.active()) {
            release(v);
        }
    }
    for (Explosion& e : mExplosions) {
        endExplosion(e);
    }
    // The next shown tick starts from the pose then, not from what passed meanwhile.
    mEventCursorValid = false;
}

void DummyItemFx::destroy() {
    stopAll();
    if (mSoundsReady) {
        for (Visual& v : mVisual) {
            v.sound.deleteObject();
            v.arrowSound.deleteObject();
        }
        mChargeSound.deleteObject();
        mSoundsReady = false;
    }
}

void DummyItemFx::updateArrow(Visual& v, double shownSeq, s8 reverb) {
    const ItemFxSlot& s = v.slot;
    const bool flying = s.state == kItemFxArrowFly;
    mDoMtx_stack_c::transS(v.pos);
    if (s.state == kItemFxArrowStuckActor) {
        mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
        mDoMtx_stack_c::transM(0.0f, 0.0f, -95.0f);
    } else {
        mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], 0);
        if (s.state == kItemFxArrowStuckBg) {
            mDoMtx_stack_c::transM(0.0f, 0.0f, -95.0f);
        } else if (s.state == kItemFxArrowRebound) {
            mDoMtx_stack_c::transM(0.0f, 0.0f, -47.5f);
        }
    }
    if (v.model != nullptr) {
        v.model->setBaseTRMtx(mDoMtx_stack_c::get());
        // It shrinks away once it sinks deep under water (procMove).
        v.scale = s.sub == kItemFxArrowBomb ? 1.0f : std::min(s.aux / 256.0f, 1.0f);
        const Vec scale = {v.scale, v.scale, v.scale};
        v.model->setBaseScale(scale);
    }
    v.placed = v.model != nullptr && s.state != kItemFxArrowSlingHit &&
               !(s.state == kItemFxArrowStuckActor && v.stateTicks >= kStuckInActorTicks);

    if (s.sub != kItemFxArrowBomb) {
        const u16 name = s.sub == kItemFxArrowSling ? 0xA55 : 0x3B8;
        if (flying) {
            setTrail(v.emitters[0], name, v.pos, v.tevStr);
            v.trailAlpha = 0xFF;
        } else if (v.trailAlpha != 0) {
            v.trailAlpha = v.trailAlpha > 0x33 ? v.trailAlpha - 51 : 0;
            if (v.trailAlpha != 0) {
                setTrail(v.emitters[0], name, v.pos, v.tevStr);
            }
        }
        if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[0])) {
            e->setGlobalAlpha(v.trailAlpha);
        }
    }

    if (s.sub == kItemFxArrowBomb) {
        v.fuse = static_cast<int16_t>(s.aux - tick16(shownSeq));
        const bool smoke = (flying || s.state == kItemFxArrowRebound) &&
                           !(s.flags & (kItemFxArrowFrozen | kItemFxArrowUnderwater));
        if (smoke && v.model != nullptr) {
            static const Vec kLocalSmoke = {-3.3f, -3.0f, 94.0f};
            mDoMtx_multVec(v.model->getBaseTRMtx(), &kLocalSmoke, &v.fxPos);
            const csXyz rot(0, s.ang[1], 0);
            v.emitters[1] = dComIfGp_particle_set(v.emitters[1], 0x1E0, &v.fxPos, &v.tevStr, &rot,
                                                  NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
            dComIfGp_particle_levelEmitterOnEventMove(v.emitters[1]);
            v.emitters[2] = dComIfGp_particle_set(v.emitters[2], 0x1DE, &v.fxPos, &v.tevStr, &rot,
                                                  NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
            dComIfGp_particle_levelEmitterOnEventMove(v.emitters[2]);
        }
    }

    if (flying && s.sub != kItemFxArrowSling && audible(v.pos)) {
        const bool charge = (s.flags & kItemFxArrowCharge) != 0;
        const u32 sound = s.sub == kItemFxArrowBomb
                              ? (charge ? Z2SE_OBJ_ARROWBOMB_FLYGAIN : Z2SE_OBJ_ARROWBOMB_FLY)
                              : (charge ? Z2SE_OBJ_ARROW_FLY_GAIN : Z2SE_OBJ_ARROW_FLY_NORMAL);
        v.arrowSound.startLevelSound(sound, 0, reverb);
    }
}

// setMoveMatrix, setRotAngle and setEffect of a thrown boomerang.
void DummyItemFx::updateBoomerang(Visual& v, s8 reverb) {
    const ItemFxSlot& s = v.slot;
    if (mTornado != nullptr && mTornadoBckReady) {
        mTornadoAng[0] = s.ang[0];
        mTornadoAng[1] = s.ang[1];
        mTornadoAng[2] = s.ang[2];
        const f32 size = std::clamp(s.aux / 256.0f, 0.0f, 4.0f);
        const Vec scale = {size, 1.0f, size};
        mTornado->setBaseScale(scale);
        mDoMtx_stack_c::transS(v.pos);
        mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
        mTornado->setBaseTRMtx(mDoMtx_stack_c::get());
        daAlink_c::simpleAnmPlay(mTornadoBtk);
        v.frame += 1.0f;
        const f32 maxFrame = mTornadoBck.getBckAnm()->getFrameMax();
        if (v.frame >= maxFrame) {
            v.frame -= maxFrame;
        }
        mTornadoBck.entry(mTornado->getModelData(), v.frame);
        mTornado->calc();
        mTornadoPlaced = v.freshTicks == 0;
        mDebug.tornado = mTornadoPlaced;
    }

    const int16_t prevSpin = v.spin;
    v.spin -= 0x1F00;
    if (prevSpin >= 0 && v.spin < 0 && audible(v.pos)) {
        mDoAud_seStart(JA_SE_LK_BOOM_FLY, &v.pos, 0, reverb);
    }
    mDoMtx_stack_c::transS(v.pos);
    mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
    mDoMtx_stack_c::YrotM(v.spin);
    mDoMtx_stack_c::XrotM(0x7FFF);
    if (v.model != nullptr) {
        v.model->setBaseTRMtx(mDoMtx_stack_c::get());
        v.placed = true;
    }

    // setEffectTraceMatrix
    MtxP trace = v.model != nullptr ? v.model->getBaseTRMtx() : mDoMtx_stack_c::get();
    static const u16 kTraceNames[] = {0x1FE, 0x1FF};
    for (int i = 0; i < 2; i++) {
        v.emitters[i] = dComIfGp_particle_set(v.emitters[i], kTraceNames[i], &v.pos, &v.tevStr);
        if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[i])) {
            e->setGlobalRTMatrix(trace);
        }
    }
    v.emitters[2] = dComIfGp_particle_set(v.emitters[2], 0x24C, &v.pos, &v.tevStr);

    // The dust where the tornado touches ground or water.
    if (mTornado != nullptr && mTornadoBckReady) {
        cXyz center(mTornado->getAnmMtx(4)[0][3], mTornado->getAnmMtx(5)[1][3],
                    mTornado->getAnmMtx(4)[2][3]);
        cXyz start(v.pos.x, v.pos.y + 50.0f, v.pos.z);
        v.gnd.SetPos(&start);
        const f32 groundY = dComIfG_Bgsp().GroundCross(&v.gnd);
        f32 surfaceY = groundY;
        bool water = false;
        if (fopAcM_wt_c::waterCheck(&start) && fopAcM_wt_c::getWaterY() > groundY) {
            surfaceY = fopAcM_wt_c::getWaterY();
            water = true;
        }
        if (surfaceY > -G_CM3D_F_INF && surfaceY > center.y) {
            center.y = surfaceY;
            csXyz angle = csXyz::Zero;
            cM3dGPla plane;
            if (!water && dComIfG_Bgsp().GetTriPla(v.gnd, &plane)) {
                angle.x = cM_atan2s(plane.mNormal.absXZ(), plane.mNormal.y);
                if (angle.x != 0) {
                    angle.y = plane.mNormal.atan2sX_Z();
                }
            }
            v.emitters[3] = dComIfGp_particle_set(v.emitters[3], 0x256, &center, &v.tevStr,
                                                  &angle, NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
            v.emitters[4] = dComIfGp_particle_set(v.emitters[4], 0x257, &center, &v.tevStr,
                                                  &angle, NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
        }
    }

    if (audible(v.pos)) {
        v.sound.startLevelSound(Z2SE_BOOM_TORNADO, 0, -1);
    }
}

// daNbomb_c::execute's matrix and setEffect, procInsectMove's clip and footsteps.
void DummyItemFx::updateBomb(Visual& v, double shownSeq, s8 reverb) {
    const ItemFxSlot& s = v.slot;
    const bool timerStop = (s.flags & kItemFxBombTimerStop) != 0;
    const bool insect = s.sub == kItemFxBombInsect;
    const bool carried = s.state == kItemFxBombCarried;
    v.scale = timerStop ? std::min(s.aux / 256.0f, 1.0f) : (insect ? 0.6f : 1.0f);
    v.fuse = timerStop ? 0 : static_cast<int16_t>(s.aux - tick16(shownSeq));
    // The sender's model matrix
    mDoMtx_stack_c::transS(v.pos);
    mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
    if (v.model == nullptr) {
        return;
    }
    v.model->setBaseTRMtx(mDoMtx_stack_c::get());
    const Vec scale = {v.scale, v.scale, v.scale};
    v.model->setBaseScale(scale);
    if (insect && mInsectBckReady) {
        // Legs still while carried (PB_WAIT), walking at 3x on a wall or floor (PB_MOVE).
        mDoExt_bckAnm& bck = carried ? mInsectWaitBck : mInsectMoveBck;
        const f32 maxFrame = bck.getBckAnm()->getFrameMax();
        const f32 prev = v.frame;
        v.frame += carried ? 1.0f : 3.0f;
        if (v.frame >= maxFrame) {
            v.frame -= maxFrame;
        }
        if (!carried && !timerStop && audible(v.pos) &&
            (v.frame < prev || (prev < 8.0f && v.frame >= 8.0f)))
        {
            v.sound.startSound(Z2SE_EN_BI_FOOTNOTE, 0, reverb);
        }
        bck.entry(v.model->getModelData(), v.frame);
    }
    v.model->calc();
    v.placed = true;

    // The ground for its shadow (draw()).
    v.groundY = -G_CM3D_F_INF;
    if (!carried) {
        cXyz start(v.pos.x, v.pos.y + 50.0f, v.pos.z);
        v.gnd.SetPos(&start);
        v.groundY = dComIfG_Bgsp().GroundCross(&v.gnd);
    }

    if (timerStop || s.state == kItemFxBombSinking) {
        return;
    }
    if (insect || s.sub == kItemFxBombWater) {
        static const u16 kNames[] = {0xA0D, 0xA0E, 0xA0F, 0xA10, 0xA11};
        mDoMtx_stack_c::copy(v.model->getAnmMtx(0));
        if (s.sub == kItemFxBombWater) {
            mDoMtx_stack_c::ZXYrotM(0, 0x4000, 0x4000);
        }
        for (int i = 0; i < 5; i++) {
            v.emitters[i] = dComIfGp_particle_set(v.emitters[i], kNames[i], &v.pos, &v.tevStr);
            if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[i])) {
                e->setGlobalRTMatrix(mDoMtx_stack_c::get());
            }
        }
    } else {
        static const Vec kLocalFuse = {0.0f, 60.0f, 0.0f};
        v.fxPosPrev = v.fxPos;
        mDoMtx_multVec(v.model->getBaseTRMtx(), &kLocalFuse, &v.fxPos);
        v.trace = v.stateTicks == 0 ? cXyz::Zero : (v.fxPos - v.fxPosPrev) * 0.5f;
        static const cXyz kFuseScale(1.8f, 1.8f, 1.8f);
        v.emitters[0] = dComIfGp_particle_set(v.emitters[0], 0x1DF, &v.fxPos, &v.tevStr, NULL,
                                              &kFuseScale, 0xFF, NULL, -1, NULL, NULL, NULL);
        if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[0])) {
            e->setParticleCallBackPtr(dPa_control_c::getParticleTracePCB());
            e->setUserWork(reinterpret_cast<uintptr_t>(&v.trace));
        }
        v.emitters[1] = dComIfGp_particle_set(v.emitters[1], 0x1DE, &v.fxPos, &v.tevStr, NULL,
                                              &kFuseScale, 0xFF, NULL, -1, NULL, NULL, NULL);
    }
    if (audible(v.pos)) {
        v.sound.startLevelSound(Z2SE_OBJ_BOMB_IGNITION, 0, reverb);
    }
}

void DummyItemFx::updateSpinner(Visual& v) {
    const ItemFxSlot& s = v.slot;
    if (v.model == nullptr) {
        return;
    }
    mDoMtx_stack_c::transS(v.pos);
    mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
    v.model->setBaseTRMtx(mDoMtx_stack_c::get());
    if (mSpinnerBckReady) {
        const f32 maxFrame = mSpinnerBck.getBckAnm()->getFrameMax();
        mSpinnerBck.entry(v.model->getModelData(), std::clamp(s.aux / kFrameScale, 0.0f, maxFrame));
    }
    v.model->calc();
    v.placed = true;

    if (s.flags & kItemFxSpinnerSparks) {
        mDoMtx_stack_c::copy(v.model->getBaseTRMtx());
        if (s.flags & kItemFxSpinnerReverse) {
            mDoMtx_stack_c::XrotM(0x7FFF);
        }
        mDoMtx_stack_c::get()[1][3] -= 10.0f;
        static const u16 kNames[2] = {0x8C5, 0x8C6};
        const csXyz rot(s.ang[0], s.ang[1], s.ang[2]);
        for (int i = 0; i < 2; i++) {
            v.emitters[i] = dComIfGp_particle_set(v.emitters[i], kNames[i], &v.pos, &v.tevStr, &rot,
                                                  NULL, 0xFF, NULL, -1, NULL, NULL, NULL);
            if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(v.emitters[i])) {
                e->setGlobalRTMatrix(mDoMtx_stack_c::get());
            }
        }
    }

    // A spinner nobody rides casts its own shadow (draw()).
    v.groundY = -G_CM3D_F_INF;
    if (!(s.flags & kItemFxSpinnerRidden)) {
        cXyz start(v.pos.x, v.pos.y, v.pos.z);
        v.gnd.SetPos(&start);
        v.groundY = dComIfG_Bgsp().GroundCross(&v.gnd);
    }
}

// daCrod_c::execute
void DummyItemFx::updateCrod(daDummyPlayer_c& dummy, Visual& v) {
    const ItemFxSlot& s = v.slot;
    if (s.state == kItemFxCrodAtRod) {
        MtxP rod = dummy.mEquipItem == dItemNo_COPY_ROD_e ? dummy.getCopyRodMtx() : nullptr;
        if (rod == nullptr) {
            cutCrodLight();
            return;
        }
        static const Vec kLocalRodPos = {81.0f, -12.5f, -12.0f};
        mDoMtx_multVec(rod, &kLocalRodPos, &v.pos);
    }
    if (v.model != nullptr) {
        mDoMtx_stack_c::transS(v.pos);
        mDoMtx_stack_c::ZXYrotM(s.ang[0], s.ang[1], s.ang[2]);
        v.model->setBaseTRMtx(mDoMtx_stack_c::get());
        daAlink_c::simpleAnmPlay(mCrodBrk);
        daAlink_c::simpleAnmPlay(mCrodBtk);
        if (mCrodBckReady) {
            mDoExt_bckAnm& bck = (s.flags & kItemFxCrodAim) ? mCrodAimBck : mCrodWaitBck;
            const f32 maxFrame = bck.getBckAnm()->getFrameMax();
            v.frame += 1.0f;
            if (v.frame >= maxFrame) {
                v.frame -= maxFrame;
            }
            bck.entry(v.model->getModelData(), v.frame);
        }
        v.model->calc();
        v.placed = true;
    }

    // create's and setLightPower's light while it glows.
    mCrodLight.mPosition = v.pos;
    mCrodLight.mColor.r = 150;
    mCrodLight.mColor.g = 170;
    mCrodLight.mColor.b = 90;
    mCrodLight.mPow = 300.0f;
    mCrodLight.mFluctuation = 50.0f;
    if (!mCrodLightOn) {
        mCrodLight.mIndex = 0;
        dKy_plight_set(&mCrodLight);
        mCrodLightOn = mCrodLight.mIndex != 0;
    }

    if (audible(v.pos)) {
        switch (s.state) {
        case kItemFxCrodFly:
            v.sound.startLevelSound(Z2SE_AL_COPYROD_THROW, 0, -1);
            break;
        case kItemFxCrodReturn:
            v.sound.startLevelSound(Z2SE_AL_COPYROD_RETURN, 0, -1);
            break;
        case kItemFxCrodAtRod:
            v.sound.startLevelSound(Z2SE_AL_COPYROD_WAIT, (s.flags & kItemFxCrodAim) ? 1 : 0, -1);
            break;
        default:
            break;
        }
    }
}

void DummyItemFx::cutCrodLight() {
    if (mCrodLightOn) {
        dKy_plight_cut(&mCrodLight);
        mCrodLight.mIndex = 0;
        mCrodLightOn = false;
    }
}

// daAlink's looping sounds of the pose shown ("ls")
void DummyItemFx::updateLevelSfx(daDummyPlayer_c& dummy, const LinkPuppetState& state,
                                 bool shown) {
    if (!shown || !state.sendsItemFx) {
        return;
    }
    bool any = false;
    for (const RemoteLevelSfx& l : state.itemFx.ls) {
        if (l.kind == 0) {
            continue;
        }
        dummy.playRemotePlayerSfx(l.id, l.kind, l.mapInfo);
        mDebug.lastLevelSfx = l.id;
        any = true;
    }
    if (any) {
        mDebug.levelSfxTicks++;
    }
}

// The boomerang in hand while it is aimed
void DummyItemFx::updateCharge(daDummyPlayer_c& dummy, const LinkPuppetState& state, bool shown) {
    const bool charge = shown && (state.visFlags & kVisBoomerangCharge) != 0 &&
                        dummy.mEquipItem == dItemNo_BOOMERANG_e && dummy.mHeldItemModel != nullptr;
    mBoomGlowPlaced = false;
    mDebug.boomCharge = charge;
    if (!charge) {
        if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(mBoomGlowEmitter)) {
            e->stopDrawParticle();
        }
        mBoomGlowEmitter = 0;
        return;
    }
    if (mBoomGlow != nullptr) {
        mBoomGlow->setBaseTRMtx(dummy.getLeftItemMatrix());
        daAlink_c::simpleAnmPlay(mBoomGlowBtk);
        mBoomGlowPlaced = true;
    }
    mDoMtx_multVecZero(dummy.mHeldItemModel->getBaseTRMtx(), &mChargePos);
    mBoomGlowEmitter = dComIfGp_particle_set(mBoomGlowEmitter, 0x740, &mChargePos, &dummy.tevStr);
    if (JPABaseEmitter* e = dComIfGp_particle_getEmitter(mBoomGlowEmitter)) {
        e->setGlobalRTMatrix(dummy.mHeldItemModel->getBaseTRMtx());
    }
    if (mSoundsReady && audible(mChargePos)) {
        mChargeSound.startLevelSound(Z2SE_BOOM_POWER_RESUME, 0, -1);
    }
}

// Plays each event once when the pose shown reaches its packet.
void DummyItemFx::fireEvents(double shownSeq, const ItemFxEventQueue& events, bool snapped,
                             bool shown) {
    const uint32_t shownTick = static_cast<uint32_t>(std::max(0.0, std::floor(shownSeq)));
    const bool playNow = shown && mEventCursorValid && !snapped;
    for (size_t i = 0; i < events.size(); i++) {
        const ItemFxEventQueue::Entry& e = events.at(i);
        if (!mEventCursorValid || static_cast<int32_t>(e.seq - mEventCursor) <= 0 ||
            static_cast<int32_t>(e.seq - shownTick) > 0)
        {
            continue;
        }
        if (playNow) {
            play(e.ev);
        } else {
            mDebug.eventsSkipped++;
        }
    }
    if (!mEventCursorValid || static_cast<int32_t>(shownTick - mEventCursor) > 0) {
        mEventCursor = shownTick;
    }
    mEventCursorValid = true;
}

void DummyItemFx::play(const ItemFxEvent& ev) {
    const cXyz pos(ev.pos[0], ev.pos[1], ev.pos[2]);
    const s8 reverb = dComIfGp_getReverb(dComIfGp_roomControl_getStayNo());
    switch (ev.type) {
    case kItemFxEvExplode:
        explode(pos, ev.rot[1], ev.arg != 0);
        break;
    case kItemFxEvHitMark: {
        const csXyz rot(ev.rot[0], ev.rot[1], 0);
        dComIfGp_setHitMark(static_cast<u16>(ev.arg), NULL, &pos, &rot, NULL, 0);
        mDebug.hitMarks++;
        break;
    }
    case kItemFxEvSound:
        mDoAud_seStart(ev.arg, &pos, static_cast<u32>(std::clamp<int>(ev.rot[0], 0, 53)), reverb);
        mDebug.sounds++;
        break;
    case kItemFxEvWater:
        // The cosmetic splash
        fopKyM_createWpillar(&pos, ev.arg / 16.0f, 0);
        mDoAud_seStart(Z2SE_CM_BODYFALL_WATER_S, &pos, 0, reverb);
        mDebug.splashes++;
        break;
    case kItemFxEvParticle: {
        const csXyz rot(ev.rot[0], ev.rot[1], 0);
        const u16 id = static_cast<u16>(ev.arg & 0xFFFF);
        const int count = static_cast<int>((ev.arg >> 16) & 0xF) + 1;
        const u32 scale16 = (ev.arg >> 20) & 0xFF;
        const f32 s = scale16 != 0 ? scale16 / 16.0f : 1.0f;
        const cXyz scale(s, s, s);
        dKy_tevstr_init(&mExplosionTev, dComIfGp_roomControl_getStayNo(), 0xFF);
        g_env_light.settingTevStruct(0, const_cast<cXyz*>(&pos), &mExplosionTev);
        for (int i = 0; i < count; i++) {
            dComIfGp_particle_set(static_cast<u16>(id + i), &pos, &mExplosionTev, &rot, &scale);
        }
        mDebug.particles++;
        break;
    }
    default:
        break;
    }
}

// procExplodeInit without the AT sphere, the noise enemies hear, the rumble and the carry cancels.
void DummyItemFx::explode(const cXyz& pos, int16_t rotY, bool underwater) {
    daAlink_c* local = daAlink_getAlinkActorClass();
    if (local == nullptr) {
        return;
    }
    const f32 s = local->getBombEffScale();
    const cXyz scale(s, s, s);
    const csXyz rot(0, rotY, 0);
    dKy_tevstr_init(&mExplosionTev, dComIfGp_roomControl_getStayNo(), 0xFF);
    g_env_light.settingTevStruct(0, const_cast<cXyz*>(&pos), &mExplosionTev);

    static const u16 kNormal[] = {0x161, 0x162, 0x163, 0x164, 0x165, 0x166, 0x167, 0x168, 0x1EC};
    static const u16 kWater[] = {0xA05, 0xA06, 0xA07, 0xA08, 0xA09, 0xA0A, 0xA0B, 0xA0C};
    static const u16 kSurface[] = {0x9FC, 0x9FD, 0x9FE, 0x9FF, 0xA00,
                                   0xA01, 0xA02, 0xA03, 0xA04};
    const u16* names = kNormal;
    int count = 9;
    u32 sound = Z2SE_OBJ_BOMB_EXPLODE;
    if (underwater) {
        if (fopAcM_wt_c::waterCheck(&pos) &&
            fopAcM_wt_c::getWaterY() - pos.y < local->getBombExplodeWaterEffectLimit())
        {
            names = kSurface;
            sound = Z2SE_OBJ_BOMB_EXP_WTRSURF;
        } else {
            names = kWater;
            count = 8;
            sound = Z2SE_OBJ_WATERBOMB_EXPLODE;
        }
    }
    for (int i = 0; i < count; i++) {
        dComIfGp_particle_setColor(names[i], &pos, &mExplosionTev, NULL, NULL, 0.0f, 0xFF, &rot,
                                   &scale, NULL, -1, NULL);
    }

    // The slot that has flared longest makes way.
    Explosion* e = &mExplosions[0];
    for (Explosion& x : mExplosions) {
        if (x.mode == 2) {
            e = &x;
            break;
        }
        if (x.mode > e->mode || (x.mode == e->mode && x.strength < e->strength)) {
            e = &x;
        }
    }
    endExplosion(*e);
    e->pos = pos;
    e->light.mPosition.set(pos.x, pos.y + 100.0f, pos.z);
    e->light.mColor.r = 100;
    e->light.mColor.g = 100;
    e->light.mColor.b = 80;
    e->light.mPow = 600.0f;
    e->light.mFluctuation = 100.0f;
    e->light.mIndex = 0;
    dKy_plight_set(&e->light);
    e->lightOn = e->light.mIndex != 0;
    e->wind.position = pos;
    e->wind.mDirection.set(0.0f, 1.0f, 0.0f);
    e->wind.mRadius = 500.0f;
    e->wind.field_0x20 = 0.0f;
    e->wind.mStrength = 0.5f;
    e->wind.field_0x24 = -1;
    dKyw_pntwind_set(&e->wind);
    e->windOn = e->wind.field_0x24 >= 0;
    e->mode = 0;
    e->strength = 0.0f;
    mDoAud_seStart(sound, &pos, 0, dComIfGp_getReverb(dComIfGp_roomControl_getStayNo()));
    mDebug.explosions++;
}

void DummyItemFx::updateExplosions() {
    mDebug.lights = 0;
    for (Explosion& e : mExplosions) {
        if (e.mode == 2) {
            continue;
        }
        e.light.mPow = e.strength * 1500.0f;
        e.wind.mStrength = e.strength;
        f32 distScale = 0.0f;
        if (camera_process_class* camera = dComIfGp_getCamera(0)) {
            const f32 dist = e.pos.abs(camera->view.lookat.eye);
            if (dist < 1500.0f) {
                distScale = dist / 1500.0f;
                distScale = 1.0f - distScale * distScale * distScale;
            }
        }
        if (e.mode == 0) {
            cLib_addCalc(&e.strength, 1.0f, 0.5f, 0.5f, 0.02f);
            if (e.strength >= 0.99f) {
                e.mode = 1;
            }
        } else {
            cLib_addCalc(&e.strength, 0.0f, 0.25f, 0.1f, 0.02f);
            if (e.strength <= 0.1f) {
                e.mode = 2;
                e.strength = 0.0f;
            }
        }
        dKy_actor_addcol_amb_set(100, 60, 50, e.strength * distScale);
        dKy_bg_addcol_amb_set(100, 60, 50, e.strength * distScale);
        if (e.mode == 2) {
            dKy_actor_addcol_set(0, 0, 0, 0.0f);
            endExplosion(e);
        } else if (e.lightOn) {
            mDebug.lights++;
        }
    }
}

// Each light and wind is cut once
void DummyItemFx::endExplosion(Explosion& e) {
    if (e.mode < 2) {
        dKy_actor_addcol_set(0, 0, 0, 0.0f);
        dKy_bg_addcol_amb_set(100, 60, 50, 0.0f);
    }
    if (e.lightOn) {
        dKy_plight_cut(&e.light);
        e.light.mIndex = 0;
        e.lightOn = false;
    }
    if (e.windOn) {
        dKyw_pntwind_cut(&e.wind);
        e.wind.field_0x24 = -1;
        e.windOn = false;
    }
    e.mode = 2;
    e.strength = 0.0f;
}

void DummyItemFx::draw(daDummyPlayer_c& dummy) {
    if (!mShown) {
        return;
    }
    for (Visual& v : mVisual) {
        if (!v.placed || v.freshTicks != 0 || v.model == nullptr) {
            continue;
        }
        g_env_light.settingTevStruct(0, &v.pos, &v.tevStr);
        switch (v.slot.kind) {
        case kItemFxArrow:
            drawArrow(v);
            break;
        case kItemFxBoomerang:
            g_env_light.setLightTevColorType_MAJI(v.model, &v.tevStr);
            mDoExt_modelUpdateDL(v.model);
            if (mTornadoPlaced) {
                g_env_light.setLightTevColorType_MAJI(mTornado, &v.tevStr);
                mDoExt_modelEntryDL(mTornado);
            }
            break;
        case kItemFxBomb:
            drawBomb(v);
            break;
        case kItemFxSpinner:
            drawSpinner(v);
            break;
        case kItemFxCrodBall:
            g_env_light.setLightTevColorType_MAJI(v.model, &v.tevStr);
            mDoExt_modelUpdateDL(v.model);
            break;
        default:
            break;
        }
    }
    if (mBoomGlowPlaced) {
        g_env_light.setLightTevColorType_MAJI(mBoomGlow, &dummy.tevStr);
        mDoExt_modelUpdateDL(mBoomGlow);
    }
}

// daArrow_c::draw
void DummyItemFx::drawArrow(Visual& v) {
    const daAlink_c* local = daAlink_getAlinkActorClass();
    const bool frozen = (v.slot.flags & kItemFxArrowFrozen) != 0;
    const bool bomb = v.slot.sub == kItemFxArrowBomb &&
                      v.model->getModelData()->getMaterialNum() > 1;
    J3DGXColorS10 color;
    color.r = color.g = color.b = color.a = 0;
    if (bomb && local != nullptr) {
        if (frozen) {
            color.r = local->getFreezeR();
            color.g = local->getFreezeG();
            color.b = local->getFreezeB();
        } else {
            const s16 et = local->getBombExplodeTime();
            const int t = v.fuse;
            f32 r;
            if (t > (et >> 1)) {
                r = fabsf(cM_fsin((t - (et >> 1)) * M_PI / (et >> 2)));
            } else if (t > (et >> 2)) {
                r = fabsf(cM_fsin((t - (et >> 2)) * M_PI / (et >> 3)));
            } else {
                r = fabsf(cM_fsin((t - (et >> 3)) * M_PI / (et >> 4)));
            }
            color.r = static_cast<u8>(r * 50.0f);
        }
        v.model->getModelData()->getMaterialNodePointer(1)->setTevColor(1, &color);
    }
    if (frozen && local != nullptr) {
        v.tevStr.TevColor.r = local->getFreezeR();
        v.tevStr.TevColor.g = local->getFreezeG();
        v.tevStr.TevColor.b = local->getFreezeB();
    }
    g_env_light.setLightTevColorType_MAJI(v.model, &v.tevStr);
    mDoExt_modelUpdateDL(v.model);
    if (bomb) {
        color.r = color.g = color.b = 0;
        v.model->getModelData()->getMaterialNodePointer(1)->setTevColor(1, &color);
    }
}

// daNbomb_c::draw
void DummyItemFx::drawBomb(Visual& v) {
    const daAlink_c* local = daAlink_getAlinkActorClass();
    const ItemFxSlot& s = v.slot;
    J3DGXColorS10 color;
    color.r = color.g = color.b = color.a = 0;
    const bool frozen = (s.flags & kItemFxBombFrozen) != 0;
    if (local != nullptr && s.state != kItemFxBombSinking) {
        if (frozen) {
            color.r = local->getFreezeR();
            color.g = local->getFreezeG();
            color.b = local->getFreezeB();
        } else if (!(s.flags & kItemFxBombTimerStop)) {
            const s16 et = local->getBombExplodeTime();
            const int t = v.fuse;
            f32 brightness;
            if (t > et) {
                brightness = 1.0f - fabsf(cM_fcos((f32)(t - et) * M_PI / (f32)(et >> 1)));
            } else if (t > (et >> 1)) {
                brightness = 1.0f - fabsf(cM_fcos((f32)(t - (et >> 1)) * M_PI / (f32)(et >> 2)));
            } else if (t > (et >> 2)) {
                brightness = fabsf(cM_fsin((f32)(t - (et >> 2)) * M_PI / (f32)(et >> 3)));
            } else {
                brightness = fabsf(cM_fsin((f32)(t - (et >> 3)) * M_PI / (f32)(et >> 4)));
            }
            color.r = static_cast<s16>(brightness * 15.0f) & 0xFF;
        }
        if (frozen) {
            v.tevStr.TevColor.r = local->getFreezeR();
            v.tevStr.TevColor.g = local->getFreezeG();
            v.tevStr.TevColor.b = local->getFreezeB();
        }
    }
    g_env_light.setLightTevColorType_MAJI(v.model, &v.tevStr);
    J3DModelData* data = v.model->getModelData();
    const bool water = s.sub == kItemFxBombWater && data->getMaterialNum() > 1;
    J3DMaterial* mat = data->getMaterialNodePointer(0);
    if (water) {
        mat->setTevColor(0, &color);
        data->getMaterialNodePointer(1)->setTevColor(0, &color);
    } else {
        mat->setTevColor(1, &color);
    }
    mDoExt_modelEntryDL(v.model);
    color.r = color.g = color.b = 0;
    if (water) {
        mat->setTevColor(0, &color);
        data->getMaterialNodePointer(1)->setTevColor(0, &color);
    } else {
        mat->setTevColor(1, &color);
    }
    if (v.groundY > -G_CM3D_F_INF && s.state != kItemFxBombCarried) {
        dComIfGd_setSimpleShadow(&v.pos, v.groundY, v.scale * 25.0f, v.gnd, 0, 1.0f,
                                 dDlst_shadowControl_c::getSimpleTex());
    }
}

// daSpinner_c::draw
void DummyItemFx::drawSpinner(Visual& v) {
    g_env_light.setLightTevColorType_MAJI(v.model, &v.tevStr);
    mDoExt_modelEntryDL(v.model);
    if (v.groundY > -G_CM3D_F_INF) {
        // Its model sits 90 above its feet
        cXyz center(v.pos.x, v.pos.y - 20.0f, v.pos.z);
        v.shadowId = dComIfGd_setShadow(v.shadowId, 1, v.model, &center, 300.0f, 0.0f, center.y,
                                        v.groundY, v.gnd, &v.tevStr, 0, 1.0f,
                                        dDlst_shadowControl_c::getSimpleTex());
    }
}

}  // namespace twili

