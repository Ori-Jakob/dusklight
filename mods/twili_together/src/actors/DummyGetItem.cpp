#include "actors/DummyGetItem.hpp"

#include "core/Log.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_resorce.h"
#include "f_op/f_op_actor_mng.h"
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "SSystem/SComponent/c_math.h"

#include <bitset>
#include <cmath>

namespace twili {
namespace {

// daDitem_c's PC estimate is 0x6720; a model that needs more gets the largest free block.
constexpr u32 kHeapSize = 0x20000;

enum class ResKind : uint8_t { Model, Btk, Bck, Brk, Btp };

// The archive's resource `idx` when its directory holds that kind: a garbage item number can
// name anything.
void* typedRes(const char* arc, s16 idx, ResKind kind) {
    dRes_info_c* info = dComIfG_getObjectResInfo(arc);
    JKRArchive* archive = info != nullptr ? info->getArchive() : nullptr;
    if (archive == nullptr || idx < 0 || static_cast<u32>(idx) >= archive->countFile()) {
        return nullptr;
    }
    const JKRArchive::SDIDirEntry* node = archive->mNodes;
    for (s32 i = 0; i < archive->countDirectory(); i++, node++) {
        const u32 first = node->first_file_index;
        if (static_cast<u32>(idx) < first || static_cast<u32>(idx) >= first + node->num_entries) {
            continue;
        }
        const u32 type = node->type;
        bool ok = false;
        switch (kind) {
        case ResKind::Model:
            ok = (type & 0xFFFFFF00u) == ('BMDR' & 0xFFFFFF00u) ||
                 (type & 0xFFFFFF00u) == ('BMWR' & 0xFFFFFF00u);
            break;
        case ResKind::Btk:
            ok = type == 'BTK ';
            break;
        case ResKind::Bck:
            ok = type == 'BCK ' || type == 'BCKS';
            break;
        case ResKind::Brk:
            ok = type == 'BRK ';
            break;
        case ResKind::Btp:
            ok = type == 'BTP ';
            break;
        }
        void* res = ok ? info->getRes(idx) : nullptr;
        if (res == nullptr || kind == ResKind::Model) {
            return res;
        }
        const s32 anmKind = static_cast<J3DAnmBase*>(res)->getKind();
        switch (kind) {
        case ResKind::Btk:
            return anmKind == 4 ? res : nullptr;
        case ResKind::Bck:
            return anmKind == 0 || anmKind == 8 || anmKind == 9 || anmKind == 16 ? res : nullptr;
        case ResKind::Brk:
            return anmKind == 5 ? res : nullptr;
        default:
            return anmKind == 2 ? res : nullptr;
        }
    }
    return nullptr;
}

// The loop frame `timer` ticks in (daItemBase_c::animPlay at rate 1 from create).
void syncFrame(mDoExt_baseAnm* anm, float timer) {
    if (anm != nullptr) {
        const f32 end = anm->getEndFrame();
        anm->setFrame(end > 0.0f ? std::fmod(timer, end) : 0.0f);
    }
}

}  // namespace

bool DummyGetItem::settled() {
    if (mLoad != Load::Loading) {
        return true;
    }
    // Once phase 1 ran, dRes counts this request until it is deleted.
    const bool counted = mPhase.id >= 1;
    const int phase = dComIfG_resLoad(&mPhase, mArc);
    if (phase == cPhs_COMPLEATE_e) {
        mLoad = Load::Loaded;
        return true;
    }
    if (phase == cPhs_ERROR_e) {
        if (counted) {
            dComIfG_deleteObjectResMain(mArc);
        }
        mLoad = Load::Failed;
        fail("its archive did not load");
        return true;
    }
    return false;
}

void DummyGetItem::fail(const char* why) {
    mDebug.failures++;
    static std::bitset<256> sLogged;
    if (!sLogged.test(mItem)) {
        sLogged.set(mItem);
        TwiliLog.warn("[dummy] item 0x{:02X} held up by a remote player is not shown: {}", mItem,
                      why);
    }
}

// As daDitem_c::create: the archive is the game's, counted per request.
bool DummyGetItem::request() {
    const char* arc = mItem != 0xFF ? dItem_data::getArcName(mItem) : nullptr;
    if (arc == nullptr || dItem_data::getBmdName(mItem) < 0) {
        mLoad = Load::Failed;
        fail("it has no model");
        return false;
    }
    mArc = arc;
    mPhase = request_of_phase_process_class{};
    mLoad = Load::Loading;
    return true;
}

// daItemBase_c::CreateItemHeap with the display item's resources, in a heap of our own.
bool DummyGetItem::build() {
    const u8 no = mItem;
    auto* data = static_cast<J3DModelData*>(typedRes(mArc, dItem_data::getBmdName(no),
                                                     ResKind::Model));
    const s16 btkId = dItem_data::getBtkName(no);
    const s16 bckId = dItem_data::getBckName(no);
    const s16 brkId = dItem_data::getBrkName(no);
    const s16 btpId = dItem_data::getBtpName(no);
    auto* btk = btkId > 0 ? static_cast<J3DAnmTextureSRTKey*>(typedRes(mArc, btkId, ResKind::Btk))
                          : nullptr;
    auto* bck = bckId > 0 ? static_cast<J3DAnmTransform*>(typedRes(mArc, bckId, ResKind::Bck))
                          : nullptr;
    auto* brk = brkId > 0 ? static_cast<J3DAnmTevRegKey*>(typedRes(mArc, brkId, ResKind::Brk))
                          : nullptr;
    auto* btp = btpId > 0 ? static_cast<J3DAnmTexPattern*>(typedRes(mArc, btpId, ResKind::Btp))
                          : nullptr;
    if (data == nullptr || (btkId > 0 && btk == nullptr) || (bckId > 0 && bck == nullptr) ||
        (brkId > 0 && brk == nullptr) || (btpId > 0 && btp == nullptr))
    {
        return false;
    }
    mTevFrm = dItem_data::getTevFrm(no);
    mBtpFrm = dItem_data::getBtpFrm(no);

    u32 diffFlags = 0x11000084;
    if (btk != nullptr) {
        diffFlags |= 0x200;
    }
    if (btp != nullptr) {
        diffFlags |= 0x20000;
    }
    const u32 modelFlags = mEffectMtx ? 0 : 0x80000;
    for (const u32 size : {kHeapSize, 0xFFFFFFFFu}) {
        mHeap = mDoExt_createSolidHeapFromGame(size, 0x20);
        if (mHeap == nullptr) {
            continue;
        }
        JKRHeap* prev = mDoExt_setCurrentHeap(mHeap);
        bool ok = (mModel = mDoExt_J3DModel__create(data, modelFlags, diffFlags)) != nullptr;
        if (ok && btk != nullptr) {
            mBtk = JKR_NEW mDoExt_btkAnm();
            ok = mBtk != nullptr && mBtk->init(data, btk, TRUE, J3DFrameCtrl::EMode_LOOP, 1.0f,
                                               0, -1);
        }
        if (ok && bck != nullptr) {
            mBck = JKR_NEW mDoExt_bckAnm();
            ok = mBck != nullptr &&
                 mBck->init(bck, TRUE, J3DFrameCtrl::EMode_LOOP, 1.0f, 0, -1, false);
        }
        if (ok && brk != nullptr) {
            mBrk = JKR_NEW mDoExt_brkAnm();
            ok = mBrk != nullptr && mBrk->init(data, brk, mTevFrm == -1, J3DFrameCtrl::EMode_LOOP,
                                               1.0f, 0, -1);
        }
        if (ok && btp != nullptr) {
            mBtp = JKR_NEW mDoExt_btpAnm();
            ok = mBtp != nullptr && mBtp->init(data, btp, TRUE, J3DFrameCtrl::EMode_LOOP, 1.0f,
                                               0, -1);
        }
        mDoExt_setCurrentHeap(prev);
        if (ok) {
            break;
        }
        mDoExt_destroySolidHeap(mHeap);
        mHeap = nullptr;
        mModel = nullptr;
        mBtk = nullptr;
        mBck = nullptr;
        mBrk = nullptr;
        mBtp = nullptr;
    }
    if (mHeap == nullptr) {
        return false;
    }
    mDoExt_adjustSolidHeap(mHeap);
    mDebug.heapUsed = mHeap->getHeapSize();
    mDebug.builds++;
    mDebug.item = no;
    if (!mTevReady) {
        dKy_tevstr_init(&mTevStr, -1, 0xFF);
        mTevReady = true;
    }
    TwiliLog.debug("[dummy] item 0x{:02X} held up from {}.arc, heap 0x{:X}", no, mArc,
                   mDebug.heapUsed);
    return true;
}

void DummyGetItem::release() {
    mPlaced = false;
    // The level emitters go out once they are no longer set.
    mEmitters[0] = mEmitters[1] = 0;
    if (mHeap != nullptr) {
        mDoExt_destroySolidHeap(mHeap);
        mHeap = nullptr;
    }
    mModel = nullptr;
    mBtk = nullptr;
    mBck = nullptr;
    mBrk = nullptr;
    mBtp = nullptr;
    if (mLoad == Load::Loaded) {
        dComIfG_resDelete(&mPhase, mArc);
    }
    mLoad = Load::Idle;
    mBuildTried = false;
    mArc = nullptr;
    mItem = 0xFF;
    mDebug.item = 0xFF;
    mDebug.loading = false;
}

void DummyGetItem::update(const RemoteGetItem& gi, const cXyz& pos, s16 yaw, bool shown) {
    mPlaced = false;
    mDebug.placed = false;
    mDebug.emitters = 0;
    const uint8_t want = gi.present() ? gi.displayItem : 0xFF;
    mDebug.wanted = want;
    mDebug.held = gi.shown();
    if (mItem != want) {
        // A load under way finishes before its archive can go back.
        if (!settled()) {
            return;
        }
        release();
        if (want == 0xFF) {
            return;
        }
        mItem = want;
        mEffectMtx = (gi.flags & kGetItemEffectMtx) != 0;
        request();
    }
    mDebug.loading = !settled();
    if (mDebug.loading) {
        return;
    }
    if (mLoad == Load::Loaded && mModel == nullptr && !mBuildTried) {
        mBuildTried = true;
        if (!build()) {
            fail("its model could not be built");
        }
    }
    if (mModel == nullptr || !gi.shown() || !shown || !(gi.scale > 0.0f)) {
        return;
    }
    place(gi, pos, yaw);
    setEffects(gi);
}

// daDitem_c::set_mtx, turned and moved with the dummy.
void DummyGetItem::place(const RemoteGetItem& gi, const cXyz& pos, s16 yaw) {
    const f32 s = cM_ssin(yaw);
    const f32 c = cM_scos(yaw);
    mPos.set(pos.x + c * gi.offset[0] + s * gi.offset[2], pos.y + gi.offset[1],
             pos.z - s * gi.offset[0] + c * gi.offset[2]);
    const cXyz scale(gi.scale, gi.scale, gi.scale);
    mModel->setBaseScale(scale);
    mDoMtx_stack_c::transS(mPos.x, mPos.y, mPos.z);
    mDoMtx_stack_c::YrotM(static_cast<s16>(yaw + gi.yaw));
    mModel->setBaseTRMtx(mDoMtx_stack_c::get());
    syncFrame(mBtk, gi.timer);
    syncFrame(mBck, gi.timer);
    if (mTevFrm == -1) {
        syncFrame(mBrk, gi.timer);
    }
    if (mBtpFrm == -1) {
        syncFrame(mBtp, gi.timer);
    }
    mEyePos = mPos;
    mEyePos.y += mModel->getModelData()->getJointNodePointer(0)->getMax()->y * 0.5f;
    mPlaced = true;
    mDebug.placed = true;
    mDebug.placedTicks++;
    mDebug.scale = gi.scale;
    for (int i = 0; i < 3; i++) {
        mDebug.offset[i] = gi.offset[i];
    }
}

// daDitem_c::actionStart's effects, from the item given (not the one shown).
void DummyGetItem::setEffects(const RemoteGetItem& gi) {
    u16 names[2] = {};
    f32 size = 1.0f;
    f32 lift = 0.0f;
    const u8 real = gi.realItem;
    if (real == dItemNo_UTAWA_HEART_e || real == dItemNo_KAKERA_HEART_e) {
        names[0] = 0x8DE;
        names[1] = 0x8DF;
    } else if (real == dItemNo_ORANGE_RUPEE_e || real == dItemNo_SILVER_RUPEE_e) {
        names[0] = 0xC14;
        size = 0.55f;
        lift = 10.0f;
    } else if (real == dItemNo_WALLET_LV3_e || real == dItemNo_CHUCHU_RARE_e) {
        names[0] = 0xC14;
        size = real == dItemNo_WALLET_LV3_e ? 1.2f : 0.8f;
        lift = 15.0f;
    } else if (real == dItemNo_FAIRY_DROP_e || real == dItemNo_DROP_BOTTLE_e) {
        names[0] = 0x8C15;
    } else if (isInsect(real)) {
        names[0] = 0xA1C;
        if (!(gi.flags & kGetItemNoLight)) {
            names[1] = 0xA1B;
        }
    }
    const cXyz at(mPos.x, mPos.y + lift, mPos.z);
    const cXyz scale(size, size, size);
    for (int i = 0; i < 2; i++) {
        if (names[i] == 0) {
            mEmitters[i] = 0;
            continue;
        }
        mEmitters[i] = dComIfGp_particle_set(mEmitters[i], names[i], &at, nullptr, &scale);
        dComIfGp_particle_levelEmitterOnEventMove(mEmitters[i]);
        if (dComIfGp_particle_getEmitter(mEmitters[i]) != nullptr) {
            mDebug.emitters++;
        }
    }
}

void DummyGetItem::stop() {
    mPlaced = false;
    mDebug.placed = false;
    mDebug.emitters = 0;
    mDebug.wanted = 0xFF;
    mDebug.held = false;
    if (settled()) {
        release();
    }
}

// daDitem_c::draw (DrawBase): the item keeps whatever list the dummy draws in.
void DummyGetItem::draw(fopAc_ac_c& dummy) {
    if (!mPlaced || mModel == nullptr) {
        return;
    }
    J3DModelData* data = mModel->getModelData();
    mTevStr.room_no = dummy.tevStr.room_no;
    mTevStr.YukaCol = dummy.tevStr.YukaCol;
    g_env_light.settingTevStruct(14, &mPos, &mTevStr);
    g_env_light.setLightTevColorType_MAJI(mModel, &mTevStr);
    // The model data is the game's: what we enter comes off again after the update.
    J3DJoint* root = data->getJointNodePointer(0);
    J3DMtxCalc* rootCalc = root->getMtxCalc();
    if (mBrk != nullptr) {
        mBrk->entry(data, mTevFrm != -1 ? static_cast<f32>(mTevFrm) : mBrk->getFrame());
    }
    if (mBtp != nullptr) {
        mBtp->entry(data, mBtpFrm != -1 ? static_cast<s16>(mBtpFrm)
                                        : static_cast<s16>(mBtp->getFrame()));
    }
    if (mBtk != nullptr) {
        mBtk->entry(data);
    }
    if (mBck != nullptr) {
        mBck->entry(data);
    }
    if (mEffectMtx) {
        // fopAcM_setEffectMtx reads the actor's eye
        const cXyz eye = dummy.eyePos;
        dummy.eyePos = mEyePos;
        fopAcM_setEffectMtx(&dummy, data);
        dummy.eyePos = eye;
    }
    mDoExt_modelUpdateDL(mModel);
    dComIfGd_setList();
    if (mBrk != nullptr) {
        mBrk->remove(data);
    }
    if (mBtp != nullptr) {
        mBtp->remove(data);
    }
    if (mBtk != nullptr) {
        mBtk->remove(data);
    }
    root->setMtxCalc(rootCalc);
}

void DummyGetItem::destroy() {
    settled();
    // A load still under way keeps its count (dComIfG_resDelete needs a finished request).
    release();
    mDebug = DummyGetItemDebug{};
}

}  // namespace twili
