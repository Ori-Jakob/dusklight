#include "actors/DummyPlayer.hpp"

#include "core/GameAccess.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "fx/WolfFx.hpp"
#include "horse/HorsePuppet.hpp"
#include "presence/RemoteFrame.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_kankyo.h"
#include "d/d_particle_name.h"
#include "d/d_resorce.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_draw_priority.h"
#include "f_pc/f_pc_leaf.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_mtx.h"
#include "res/Object/Alink.h"
#include "res/Object/CWShd.h"
#include "res/Object/HyShd.h"
#include "res/Object/Kmdl.h"
#include "res/Object/SWShd.h"
#include "res/Object/Wmdl.h"
#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2Audience.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace twili {

static constexpr const char* kLinkArcPath = "/res/Object/Kmdl.arc";
static constexpr const char* kMagicArmorArcPath = "/res/Object/Mmdl.arc";
static constexpr const char* kZoraArcPath = "/res/Object/Zmdl.arc";
static constexpr const char* kAlinkArcPath = "/res/Object/Alink.arc";
static constexpr const char* kHylianShieldArcPath = "/res/Object/HyShd.arc";
static constexpr const char* kOrdonShieldArcPath = "/res/Object/CWShd.arc";
static constexpr const char* kWoodShieldArcPath = "/res/Object/SWShd.arc";
static constexpr const char* kWolfArcPath = "/res/Object/Wmdl.arc";
static constexpr const char* kCasualArcPath = "/res/Object/Bmdl.arc";
static constexpr u16 kNoAnm = 0xFFFF;
static constexpr u16 kSwordEquipItem = 0x0103;
static constexpr u32 kPlayerBckBufferSize = 0x10800;
static constexpr u32 kAnmPackBufferSize = 0x2C00;
static constexpr int kAnmPackCount = 3;
static constexpr u16 kMaxDemoAnmArcNo = 8;
// Every clip daAlink asks of a demo archive has a resource index of at most 0x2E
static constexpr u16 kMaxDemoAnmRes = 0xFF;
static constexpr u16 kFirstAlAnmBck = 0x8;
static constexpr u16 kLastAlAnmBck = 0x30D;
static constexpr u32 kDummyLinkHeapSize = 0x600000;
static constexpr u32 kDummyLinkHeapFlags = 0x80000000 | 0x40000000 | 0x20000000;
// res/Object/{Mmdl,Zmdl,Bmdl}.h cannot be included next to Kmdl.h
static constexpr u16 kMmdlBmdAlBootsH = 0x0D;
static constexpr u16 kMmdlBmdAlFace = 0x0E;
static constexpr u16 kMmdlBmdAlHands = 0x0F;
static constexpr u16 kMmdlBmdAlSwb = 0x10;
static constexpr u16 kMmdlBmdMl = 0x11;
static constexpr u16 kMmdlBmdMlHead = 0x12;
static constexpr u16 kMmdlBrkMlBodyPowerDown = 0x15;
static constexpr u16 kMmdlBrkMlBodyPowerUpA = 0x16;
static constexpr u16 kMmdlBrkMlBodyPowerUpB = 0x17;
static constexpr u16 kMmdlBrkMlHeadPowerDown = 0x18;
static constexpr u16 kMmdlBrkMlHeadPowerUpA = 0x19;
static constexpr u16 kMmdlBrkMlHeadPowerUpB = 0x1A;
static constexpr u16 kZmdlBmdAlBootsH = 0x0C;
static constexpr u16 kZmdlBmdAlHands = 0x0D;
static constexpr u16 kZmdlBmdAlSwb = 0x0E;
static constexpr u16 kZmdlBmdZl = 0x0F;
static constexpr u16 kZmdlBmdZlFace = 0x10;
static constexpr u16 kZmdlBmdZlHead = 0x11;
static constexpr u16 kBmdlBmdAlBootsH = 0x0C;
static constexpr u16 kBmdlBmdAlFace = 0x0D;
static constexpr u16 kBmdlBmdAlSwb = 0x0E;
static constexpr u16 kBmdlBmdBl = 0x0F;
static constexpr u16 kBmdlBmdBlHands = 0x10;
static constexpr u16 kBmdlBmdBlHead = 0x11;
static constexpr f32 kDummyRemoteMorfFrames = 3.0f;
static constexpr u16 kDummyRemoteMorfStartJoint = 0;
static constexpr f32 kRemotePlayerSfxMaxDistance = 3500.0f;
static constexpr f32 kRemotePlayerSfxMaxDistanceSq =
    kRemotePlayerSfxMaxDistance * kRemotePlayerSfxMaxDistance;
static constexpr f32 kDummyWaitRootTransX = 1.24279f;
static constexpr f32 kDummyWaitRootTransZ = 5.0f;
static constexpr f32 kDummyHalfAtnRootTransX = 3.5f;
static constexpr f32 kDummyHalfAtnRootTransZ = -7.0f;
// l_horseBaseAnime (d_a_alink.cpp)
static const cXyz kDummyHorseBaseAnime(-1.24279f, 225.7f, 1.81f - 5.0f);
static constexpr f32 kDummyWolfRootTransX = 1.0f;  // l_wolfBaseAnime (d_a_alink.cpp)
static constexpr f32 kDummyWolfRootTransZ = -28.497932f;
// The fur that grows on Link's head while he transforms (setMetamorphoseModel).
static constexpr u16 kMetamorphoseItem = 0x106;
// A blade of the grass that calls a hawk or the horse (setGrassWhistleModel).
static constexpr u16 kGrassWhistleItem = 0x104;
static constexpr u8 kDummyBaseHoldTicks = 20;
// Chance per tick that the idle face blinks (setFaceBtp's rate for FMABA01).
static constexpr f32 kDummyBlinkChance = 0.012f;
static constexpr u16 kNoItemBck = 0xFFFF;

enum DummyProjectileType : uint8_t {
    DUMMY_PROJECTILE_NONE = 0,
    DUMMY_PROJECTILE_ARROW = 1,
    DUMMY_PROJECTILE_BOMB_ARROW = 2,
    DUMMY_PROJECTILE_SLING = 3,
};

// PvP hurtbox (pvp/)
static constexpr u32 kDummyTgType = AT_TYPE_NORMAL_SWORD | AT_TYPE_MASTER_SWORD |
                                    AT_TYPE_SHIELD_ATTACK | AT_TYPE_HOOKSHOT |
                                    AT_TYPE_HEAVY_BOOTS | AT_TYPE_IRON_BALL | AT_TYPE_MIDNA_LOCK |
                                    AT_TYPE_WOLF_CUT_TURN | (u32)AT_TYPE_WOLF_ATTACK |
                                    AT_TYPE_ARROW | AT_TYPE_SLINGSHOT | AT_TYPE_BOOMERANG |
                                    AT_TYPE_BOMB | AT_TYPE_SPINNER | AT_TYPE_HORSE;

static const dCcD_SrcCyl l_dummyTgCylSrc = {
    {
        {0, {{0, 0, 0}, {kDummyTgType, 0x3}, 0}},
        {dCcD_SE_NONE, 0, 0, dCcD_MTRL_NONE, {0}},
        // Hit mark 6 like the real TG (l_cylSrc).
        {dCcD_SE_NONE, CcG_Tg_UNK_MARK_6, 0, dCcD_MTRL_NONE, {0x2}},
        {0},
    },
    {{{0.0f, 0.0f, 0.0f}, 35.0f, 180.0f}},  // l_cylSrc's shape
};

static const dCcD_SrcSph l_dummyTgSphSrc = {
    {
        {0, {{0, 0, 0}, {kDummyTgType, 0x3}, 0}},
        {dCcD_SE_NONE, 0, 0, dCcD_MTRL_NONE, {0}},
        {dCcD_SE_NONE, CcG_Tg_UNK_MARK_6, 0, dCcD_MTRL_NONE, {0x2}},
        {0},
    },
    {{{0.0f, 0.0f, 0.0f}, 40.0f}},  // l_sphSrc's shape: the wolf's head
};

static bool isRemotePlayerAudioAudible(const fopAc_ac_c* actor) {
    // The local player does not exist while a stage is loading.
    return dComIfGp_getPlayer(0) != nullptr &&
           fopAcM_searchPlayerDistanceXZ2(actor) <= kRemotePlayerSfxMaxDistanceSq;
}

static bool isValidSwordItem(uint8_t item) {
    switch (item) {
    case dItemNo_SWORD_e:
    case dItemNo_MASTER_SWORD_e:
    case dItemNo_WOOD_STICK_e:
    case dItemNo_LIGHT_SWORD_e:
        return true;
    default:
        return false;
    }
}

static bool isValidShieldItem(uint8_t item) {
    switch (item) {
    case dItemNo_WOOD_SHIELD_e:
    case dItemNo_SHIELD_e:
    case dItemNo_HYLIA_SHIELD_e:
        return true;
    default:
        return false;
    }
}

static uint8_t normalizeSwordItem(uint8_t item) {
    return isValidSwordItem(item) ? item : dItemNo_NONE_e;
}

static uint8_t normalizeShieldItem(uint8_t item) {
    return isValidShieldItem(item) ? item : dItemNo_NONE_e;
}

static uint8_t normalizeClothesItem(uint8_t item) {
    switch (item) {
    case dItemNo_WEAR_CASUAL_e:
    case dItemNo_WEAR_KOKIRI_e:
    case dItemNo_WEAR_ZORA_e:
    case dItemNo_ARMOR_e:
        return item;
    default:
        return dItemNo_WEAR_KOKIRI_e;
    }
}

static bool isRemoteHeldItemModelItem(uint16_t item) {
    return item == dItemNo_PACHINKO_e ||
           item == kMetamorphoseItem ||
           item == dItemNo_COPY_ROD_e ||
           item == dItemNo_KANTERA_e ||
           item == dItemNo_BOOMERANG_e ||
           item == dItemNo_IRONBALL_e ||
           item == dItemNo_HORSE_FLUTE_e ||
           item == kGrassWhistleItem ||
           daPy_py_c::checkBowItem(item) ||
           daPy_py_c::checkBottleItem(item) ||
           daPy_py_c::checkHookshotItem(item);
}

static bool isRemoteHeldItem(uint16_t item) {
    return item != dItemNo_NONE_e && item != kSwordEquipItem &&
           (isRemoteHeldItemModelItem(item) || item == twili::wolffx::kLockDomeItem);
}

static uint16_t normalizeDummyEquipItem(uint16_t item, uint8_t swordItem) {
    if (item == kSwordEquipItem && isValidSwordItem(swordItem)) {
        return kSwordEquipItem;
    }

    // The dome is refused on a human body in syncRemoteHeldItemModel.
    return isRemoteHeldItemModelItem(item) || item == twili::wolffx::kLockDomeItem
               ? item
               : dItemNo_NONE_e;
}

static bool shouldForceRemoteSwordInHand(const LinkPuppetState& state, uint8_t swordItem) {
    return isValidSwordItem(swordItem) &&
           (state.swordBlurActive || state.swordChargeActive ||
            state.cutType != daPy_py_c::CUT_TYPE_NONE);
}

class ScopedSelectEquip {
public:
    ScopedSelectEquip(uint8_t clothes, uint8_t sword, uint8_t shield)
        : mStatus(g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA()),
          mOldClothes(mStatus.getSelectEquip(COLLECT_CLOTHING)),
          mOldSword(mStatus.getSelectEquip(COLLECT_SWORD)),
          mOldShield(mStatus.getSelectEquip(COLLECT_SHIELD)) {
        mStatus.setSelectEquip(COLLECT_CLOTHING, normalizeClothesItem(clothes));
        mStatus.setSelectEquip(COLLECT_SWORD, normalizeSwordItem(sword));
        mStatus.setSelectEquip(COLLECT_SHIELD, normalizeShieldItem(shield));
    }

    ~ScopedSelectEquip() {
        mStatus.setSelectEquip(COLLECT_CLOTHING, mOldClothes);
        mStatus.setSelectEquip(COLLECT_SWORD, mOldSword);
        mStatus.setSelectEquip(COLLECT_SHIELD, mOldShield);
    }

private:
    dSv_player_status_a_c& mStatus;
    uint8_t mOldClothes;
    uint8_t mOldSword;
    uint8_t mOldShield;
};

// checkMagicArmorHeavy (draw()'s damage flash) reads the save's rupee count.
class ScopedRemoteRupees {
public:
    ScopedRemoteRupees(bool armor, bool drained) : mOld(dComIfGs_getRupee()), mActive(armor) {
        if (mActive) {
            dComIfGs_setRupee(drained ? 0 : (std::max)(mOld, static_cast<u16>(1)));
        }
    }

    ~ScopedRemoteRupees() {
        if (mActive) {
            dComIfGs_setRupee(mOld);
        }
    }

private:
    u16 mOld;
    bool mActive;
};

// daAlink_c reads a transformation's look from its proc
class ScopedRemoteMetamorphose {
public:
    ScopedRemoteMetamorphose(daAlink_c& link, bool enable, s16 tev, f32 hatScale)
        : mLink(link), mEnable(enable), mOldProcID(link.mProcID),
          mOldTev(link.mProcVar3.field_0x300e), mOldHatScale(link.field_0x347c) {
        if (mEnable) {
            mLink.mProcID = daAlink_c::PROC_METAMORPHOSE;
            mLink.mProcVar3.field_0x300e = tev;
            mLink.field_0x347c = hatScale;
        }
    }

    ~ScopedRemoteMetamorphose() {
        if (mEnable) {
            mLink.mProcID = mOldProcID;
            mLink.mProcVar3.field_0x300e = mOldTev;
            mLink.field_0x347c = mOldHatScale;
        }
    }

private:
    daAlink_c& mLink;
    bool mEnable;
    u16 mOldProcID;
    s16 mOldTev;
    f32 mOldHatScale;
};

class ScopedCameraAttentionMask {
public:
    ScopedCameraAttentionMask(int cameraId, u32 mask) : mCameraId(cameraId), mMask(mask) {
        if (mCameraId != 0) {
            return;
        }

        mOldStatus = dComIfGp_getCameraAttentionStatus(mCameraId);
        if ((mOldStatus & mMask) == 0) {
            return;
        }

        g_dComIfG_gameInfo.play.setCameraAttentionStatus(mCameraId, mOldStatus & ~mMask);
        mRestore = true;
    }

    ~ScopedCameraAttentionMask() {
        if (mRestore) {
            g_dComIfG_gameInfo.play.setCameraAttentionStatus(mCameraId, mOldStatus);
        }
    }

private:
    int mCameraId;
    u32 mMask;
    u32 mOldStatus = 0;
    bool mRestore = false;
};

void restoreLocalLinkAudioPtr() {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link != nullptr) {
        Z2CreatureLink::mLinkPtr = &link->mZ2Link;
        if (Z2Audience* audience = Z2GetAudience()) {
            audience->getLinkMic()->setPosPtr(link->mZ2Link.getCurrentPos());
        }
    }
}

class ScopedDummyLinkAudioPtr {
public:
    explicit ScopedDummyLinkAudioPtr(Z2CreatureLink* link) : mPrevious(Z2CreatureLink::mLinkPtr) {
        Z2CreatureLink::mLinkPtr = link;
        if (link != nullptr) {
            if (Z2Audience* audience = Z2GetAudience()) {
                audience->getLinkMic()->setPosPtr(link->getCurrentPos());
            }
        }
    }

    ~ScopedDummyLinkAudioPtr() {
        if (mPrevious != nullptr) {
            Z2CreatureLink::mLinkPtr = mPrevious;
        } else {
            restoreLocalLinkAudioPtr();
        }
    }

private:
    Z2CreatureLink* mPrevious;
};

static u32 getArchiveNodeType(JKRArchive* archive, u16 fileIndex) {
    if (!archive) {
        return 'BMDR';
    }

    JKRArchive::SDIDirEntry* node = archive->mNodes;
    for (s32 i = 0; i < archive->countDirectory(); i++, node++) {
        const u32 first = node->first_file_index;
        if (fileIndex >= first && fileIndex < first + node->num_entries) {
            return node->type;
        }
    }

    return 'BMDR';
}

static bool isWarpBmdTag(u32 tag) {
    return tag == 'BMWR' || tag == 'BMWE';
}

struct PrivateBmdCacheEntry {
    JKRArchive* archive;
    u16 index;
    u32 tag;
    J3DModelData* data;
};
static PrivateBmdCacheEntry s_privateBmdCache[64];
static int s_privateBmdCacheCount = 0;

static void resetPrivateBmdCache() {
    s_privateBmdCacheCount = 0;
}

static J3DModelData* loadPrivateBmd(JKRArchive* archive, u16 bmdIndex, u32* outTag = nullptr) {
    if (!archive || !archive->isFileEntry(bmdIndex)) {
        return nullptr;
    }

    for (int i = 0; i < s_privateBmdCacheCount; i++) {
        const PrivateBmdCacheEntry& entry = s_privateBmdCache[i];
        if (entry.archive == archive && entry.index == bmdIndex) {
            if (outTag) {
                *outTag = entry.tag;
            }
            return entry.data;
        }
    }

    void* raw = archive->getIdxResource(bmdIndex);
    if (!raw) {
        return nullptr;
    }

    const u32 tag = getArchiveNodeType(archive, bmdIndex);
    if (outTag) {
        *outTag = tag;
    }
    J3DModelData* data = dRes_info_c::loaderBasicBmd(tag, raw);
    if (data && s_privateBmdCacheCount < (int)ARRAY_SIZE(s_privateBmdCache)) {
        s_privateBmdCache[s_privateBmdCacheCount++] = {archive, bmdIndex, tag, data};
    } else if (data) {
        TwiliLog.warn("[dummy] private BMD cache full, index {} may not be reused", bmdIndex);
    }
    return data;
}

enum PrivateArc {
    kArcKmdl, kArcMmdl, kArcZmdl, kArcBmdl, kArcAlink, kArcHyShd, kArcCWShd, kArcSWShd, kArcWmdl,
    kArcCount,
};
constexpr const char* kPrivateArcPaths[kArcCount] = {
    kLinkArcPath, kMagicArmorArcPath, kZoraArcPath, kCasualArcPath, kAlinkArcPath,
    kHylianShieldArcPath, kOrdonShieldArcPath, kWoodShieldArcPath, kWolfArcPath,
};
// About 0x270000 decompressed; trimmed once they are mounted.
constexpr u32 kPrivateArcHeapSize = 0x400000;

static J3DModel* makeModelEx(JKRArchive* archive, u16 bmdId, u32 modelFlags, u32 diffFlags) {
    u32 tag = 'BMDR';
    J3DModelData* data = loadPrivateBmd(archive, bmdId, &tag);
    if (!data) {
        return nullptr;
    }

    const bool warpMaterial = isWarpBmdTag(tag);
    if (warpMaterial) {
        dRes_info_c::onWarpMaterial(data);
        diffFlags |= 0x2000400;
    }

    J3DModel* model = mDoExt_J3DModel__create(data, modelFlags, diffFlags | 0x11000084);
    if (warpMaterial) {
        dRes_info_c::offWarpMaterial(data);
    }
    return model;
}

static J3DModel* makeModel(JKRArchive* archive, u16 bmdId) {
    return makeModelEx(archive, bmdId, 0x80000, 0);
}

// changeWolf's PC fix
static void clearTextureMaxLod(J3DModel* model, const char* texName) {
    J3DModelData* data = model->getModelData();
    J3DTexture* tex = data->getTexture();
    JUTNameTab* names = data->getTextureName();
    if (!tex || !names) {
        return;
    }

    for (u16 i = 0; i < tex->getNum(); i++) {
        const char* name = names->getName(i);
        if (name && std::strcmp(name, texName) == 0) {
            tex->getResTIMG(i)->maxLOD = 0;
        }
    }
}

// changeLink's fix of the same kind.
static void clearEyeTextureMaxLod(J3DModel* faceModel) {
    if (faceModel) {
        clearTextureMaxLod(faceModel, "al_eyeball");
        clearTextureMaxLod(faceModel, "highlight02");
        clearTextureMaxLod(faceModel, "eye_kage01");
    }
}

// daAlink_kandelaarModelCallBack for the dummy's own lantern
static int daDummyPlayer_kandelaarModelCallBack(J3DJoint*, int calcTiming) {
    if (calcTiming == 0) {
        reinterpret_cast<daDummyPlayer_c*>(j3dSys.getModel()->getUserArea())
            ->kandelaarModelCallBack();
    }
    return 1;
}

static J3DAnmBase* loadPrivateAnm(JKRArchive* archive, u16 resId) {
    if (!archive || !archive->isFileEntry(resId)) {
        return nullptr;
    }

    void* raw = archive->getIdxResource(resId);
    if (!raw) {
        return nullptr;
    }
    return J3DAnmLoaderDataBase::load(raw);
}

static J3DAnmTextureSRTKey* loadPrivateBtk(JKRArchive* archive, u16 resId) {
    J3DAnmBase* anm = loadPrivateAnm(archive, resId);
    if (!anm || anm->getKind() != 4) {
        return nullptr;
    }
    return static_cast<J3DAnmTextureSRTKey*>(anm);
}

static J3DAnmTevRegKey* loadPrivateBrk(JKRArchive* archive, u16 resId) {
    J3DAnmBase* anm = loadPrivateAnm(archive, resId);
    if (!anm || anm->getKind() != 5) {
        return nullptr;
    }
    return static_cast<J3DAnmTevRegKey*>(anm);
}

// Like the BMDs, J3DAnmLoader swaps a clip's key data in place
static J3DAnmTransform* loadPrivateBck(JKRArchive* archive, u16 resId) {
    J3DAnmBase* anm = loadPrivateAnm(archive, resId);
    const s32 kind = anm != nullptr ? anm->getKind() : -1;
    if (kind != 0 && kind != 8 && kind != 9) {  // J3DAnmTransform, ...Key, ...Full
        return nullptr;
    }
    return static_cast<J3DAnmTransform*>(anm);
}

static void bindTexMtxAnm(J3DModel* model, J3DAnmTextureSRTKey* anm) {
    if (!model || !anm) {
        return;
    }

    J3DModelData* data = model->getModelData();
    anm->searchUpdateMaterialID(data);
    data->entryTexMtxAnimator(anm);
    anm->setFrame(0.0f);
}

static void bindTevRegAnm(J3DModel* model, J3DAnmTevRegKey* anm) {
    if (!model || !anm) {
        return;
    }

    J3DModelData* data = model->getModelData();
    anm->searchUpdateMaterialID(data);
    data->entryTevRegAnimator(anm);
    anm->setFrame(0.0f);
}

static void setMaterialShapeVisible(J3DModel* model, u16 materialIndex, bool visible) {
    if (!model) {
        return;
    }

    J3DModelData* data = model->getModelData();
    if (!data || materialIndex >= data->getMaterialNum()) {
        return;
    }

    J3DShape* shape = data->getMaterialNodePointer(materialIndex)->getShape();
    if (!shape) {
        return;
    }

    if (visible) {
        shape->show();
    } else {
        shape->hide();
    }
}

static J3DShape* getMaterialShape(J3DModel* model, u16 materialIndex) {
    if (!model) {
        return nullptr;
    }

    J3DModelData* data = model->getModelData();
    if (!data || materialIndex >= data->getMaterialNum()) {
        return nullptr;
    }

    J3DMaterial* material = data->getMaterialNodePointer(materialIndex);
    return material ? material->getShape() : nullptr;
}

static u16 getMaterialCount(J3DModel* model) {
    J3DModelData* data = model ? model->getModelData() : nullptr;
    return data ? data->getMaterialNum() : 0;
}

static void setShapeVisible(J3DShape* shape, bool visible) {
    if (!shape) {
        return;
    }

    if (visible) {
        shape->show();
    } else {
        shape->hide();
    }
}

static void hideHandMaterials(J3DModel* handModel) {
    if (!handModel) {
        return;
    }

    J3DModelData* data = handModel->getModelData();
    if (!data) {
        return;
    }

    const u16 count = (std::min)(data->getMaterialNum(), static_cast<u16>(11));
    for (u16 i = 0; i < count; i++) {
        data->getMaterialNodePointer(i)->getShape()->hide();
    }
}

static void setClampedAnmFrame(J3DAnmBase* anm, float frame) {
    if (!anm) {
        return;
    }

    const s16 maxFrame = anm->getFrameMax();
    if (maxFrame <= 0 || !std::isfinite(frame)) {
        anm->setFrame(0.0f);
        return;
    }

    const float max = static_cast<float>(maxFrame);
    if (frame > max) {
        frame = max;
    } else if (frame < 0.0f) {
        frame = 0.0f;
    }
    anm->setFrame(frame);
}

static void advanceSimpleAnm(J3DAnmBase* anm) {
    if (!anm) {
        return;
    }

    const s16 maxFrame = anm->getFrameMax();
    if (maxFrame <= 0) {
        anm->setFrame(0.0f);
        return;
    }

    float frame = anm->getFrame() + 1.0f;
    if (frame >= maxFrame) {
        if (anm->getAttribute() == 2) {
            frame -= maxFrame;
        } else {
            frame = static_cast<float>(maxFrame);
        }
    }
    anm->setFrame(frame);
}

static bool isMasterSwordItem(uint8_t swordItem) {
    return swordItem == dItemNo_MASTER_SWORD_e || swordItem == dItemNo_LIGHT_SWORD_e;
}

static bool isValidAnm(uint16_t anmId) {
    if (anmId == 0 || anmId == kNoAnm) {
        return false;
    }

    const u16 arcId = (anmId >> 12) & 0xF;
    const u16 resId = anmId & 0xFFF;
    if (resId == 0 || arcId > kMaxDemoAnmArcNo) {
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

static void traceInitialApply(uint32_t clientId, bool enabled, const char* phase) {
    if (enabled) {
        TwiliLog.debug("[dummy {}] {}", clientId, phase);
    }
}

static bool isTransformAnm(J3DAnmBase* anm) {
    if (!anm) {
        return false;
    }

    const s32 kind = anm->getKind();
    return kind == 0 || kind == 8 || kind == 9;
}

static bool splitDemoArcId(u16& arcNo, u16& resId) {
    if (arcNo != kNoAnm) {
        return true;
    }

    const u16 arcId = (resId >> 12) & 0xF;
    resId &= 0xFFF;
    if (arcId == 0) {
        return true;
    }

    if (arcId > kMaxDemoAnmArcNo) {
        return false;
    }

    arcNo = arcId;
    return true;
}

// `heapTouched` tells whether the heap's clip was replaced
static J3DAnmTransform* loadPlayerBck(daPy_anmHeap_c& heap, u16 anmId, u32 bufferSize,
                                      bool& heapTouched) {
    heapTouched = false;
    if (!isValidAnm(anmId)) {
        return nullptr;
    }

    u16 arcNo = kNoAnm;
    u16 resId = anmId;
    if (!splitDemoArcId(arcNo, resId)) {
        return nullptr;
    }

    if (arcNo == kNoAnm) {
        JKRArchive* archive = dComIfGp_getAnmArchive();
        if (!archive || !archive->isFileEntry(resId)) {
            return nullptr;
        }

        heap.setBufferSize(bufferSize);
        heapTouched = true;
        J3DAnmBase* anm = static_cast<J3DAnmBase*>(heap.loadDataIdx(resId));
        return isTransformAnm(anm) ? static_cast<J3DAnmTransform*>(anm) : nullptr;
    }

    // Looked up without daPy_anmHeap_c::loadData, which reports every miss through getRes
    J3DAnmBase* anm =
        static_cast<J3DAnmBase*>(twili::Session::residentPlayerArcAnm(arcNo, resId));
    if (!isTransformAnm(anm)) {
        return nullptr;
    }
    // Names the clip in the heap as the player's own load does
    heap.loadDataDemoRID(resId, arcNo);
    return static_cast<J3DAnmTransform*>(anm);
}

static float clampRatio(float ratio) {
    if (!std::isfinite(ratio) || ratio < 0.0f) {
        return 0.0f;
    }
    if (ratio > 1.0f) {
        return 1.0f;
    }
    return ratio;
}

static uint16_t dominantRemoteAnmId(const uint16_t anms[kAnmPackCount],
                                    const float ratios[kAnmPackCount]) {
    uint16_t firstValid = kNoAnm;
    uint16_t bestAnm = kNoAnm;
    float bestRatio = 0.0f;

    for (int i = 0; i < kAnmPackCount; i++) {
        if (!isValidAnm(anms[i])) {
            continue;
        }

        if (firstValid == kNoAnm) {
            firstValid = anms[i];
        }

        const float ratio = clampRatio(ratios[i]);
        if (ratio > bestRatio) {
            bestRatio = ratio;
            bestAnm = anms[i];
        }
    }

    return bestAnm != kNoAnm ? bestAnm : firstValid;
}

static int dominantRemoteAnmPack(const uint16_t anms[kAnmPackCount],
                                 const float ratios[kAnmPackCount]) {
    int firstValid = -1;
    int bestPack = -1;
    float bestRatio = 0.0f;

    for (int i = 0; i < kAnmPackCount; i++) {
        if (!isValidAnm(anms[i])) {
            continue;
        }

        if (firstValid < 0) {
            firstValid = i;
        }

        const float ratio = clampRatio(ratios[i]);
        if (ratio > bestRatio) {
            bestRatio = ratio;
            bestPack = i;
        }
    }

    return bestPack >= 0 ? bestPack : firstValid;
}

static u32 packBufferSize(bool upper, int idx) {
    if (idx == 0) {
        return upper ? (kAnmPackBufferSize * 3) : kPlayerBckBufferSize;
    }
    return kAnmPackBufferSize;
}

static uint8_t sanitizeLeftHandIndex(uint8_t index) {
    if (index == 0xFE || index == 0xFB || index == 0x64) {
        return index;
    }
    return index < 11 ? index : 0xFE;
}

static uint8_t sanitizeRightHandIndex(uint8_t index) {
    if (index == 0xFE || index == 0xFB) {
        return index;
    }
    return index < 11 ? index : 0xFE;
}

static uint8_t sanitizeHandIndexForModel(uint8_t index, J3DModel* handModel) {
    if (index == 0xFE || index == 0xFB || index == 0x64) {
        return index;
    }
    const u16 materialCount = getMaterialCount(handModel);
    return index < materialCount ? index : 0xFE;
}

static uint8_t sanitizeLeftItemHandOverride(uint8_t index, J3DModel* handModel) {
    if (index == 0xFF || index == 0xFE || index == 0xFB || index == 0x65 ||
        index == 0x67)
    {
        return index;
    }

    const u16 materialCount = getMaterialCount(handModel);
    return index < materialCount ? index : 0xFF;
}

static uint8_t sanitizeRightItemHandOverride(uint8_t index, J3DModel* handModel) {
    if (index == 0xFF || index == 0xFE || index == 0xFB || index == 0x65) {
        return index;
    }

    const u16 materialCount = getMaterialCount(handModel);
    return index < materialCount ? index : 0xFF;
}

static uint8_t sanitizeGripHandOverride(uint8_t index, J3DModel* handModel) {
    if (index == 0xFF || index == 0xFE || index == 0xFB) {
        return index;
    }

    const u16 materialCount = getMaterialCount(handModel);
    return index < materialCount ? index : 0xFF;
}

static u16 sanitizeItemJoint(u16 joint, J3DModel* model, u16 fallback) {
    J3DModelData* data = model ? model->getModelData() : nullptr;
    return data && joint < data->getJointNum() ? joint : fallback;
}

static f32 sanitizeItemBckFrame(f32 frame) {
    return std::isfinite(frame) && frame > 0.0f ? frame : 0.0f;
}

static bool isRemoteBowItemBck(uint16_t bckId, uint16_t equipItem) {
    if (bckId == kNoItemBck) {
        return false;
    }

    if (equipItem == dItemNo_PACHINKO_e) {
        return bckId == dRes_ID_ALANM_BCK_PWAIT_e ||
               bckId == dRes_ID_ALANM_BCK_PRELORD_e ||
               bckId == dRes_ID_ALANM_BCK_PSHOOT_e;
    }

    if (daPy_py_c::checkBowItem(equipItem)) {
        return bckId == dRes_ID_ALANM_BCK_BVJMPCL_e ||
               bckId == dRes_ID_ALANM_BCK_BVJMPCH_e ||
               bckId == dRes_ID_ALANM_BCK_BARELORD_e ||
               bckId == dRes_ID_ALANM_BCK_BARELORDTAME_e ||
               bckId == dRes_ID_ALANM_BCK_BASHOOT_e ||
               bckId == dRes_ID_ALANM_BCK_BASHOOTTAME_e;
    }

    return false;
}

static int daDummyPlayer_createHeap(fopAc_ac_c* i_this) {
    return static_cast<daDummyPlayer_c*>(i_this)->createHeap();
}

int daDummyPlayer_c::createHeap() {
    // The loaders swap the raw files in place, so the archives back one attempt only.
    if (mDummyArchivesUsed) {
        return FALSE;
    }
    mDummyArchivesUsed = true;
    resetPrivateBmdCache();
    const int result = createHeapImpl();
    resetPrivateBmdCache();
    return result;
}

cPhs_Step daDummyPlayer_c::loadPrivateArchives() {
    switch (mDummyArchives.update(this, kPrivateArcPaths, kArcCount, kPrivateArcHeapSize)) {
    case twili::PrivateArchives::State::Idle:
        if (mDummyArchives.failed()) {
            TwiliLog.warn("[dummy {}] create: no heap for the private archives", mDummyClientId);
            return cPhs_ERROR_e;
        }
        return cPhs_INIT_e;
    case twili::PrivateArchives::State::Loading:
        return cPhs_INIT_e;
    case twili::PrivateArchives::State::Ready:
        break;
    }
    return cPhs_COMPLEATE_e;
}

void daDummyPlayer_c::clearPrivateModelPointers() {
    mpDummyKokiriLinkModel = mpDummyKokiriFaceModel = mpDummyKokiriHatModel = nullptr;
    mpDummyKokiriHandModel = mpDummyKokiriWoodSwordModel = nullptr;
    mpDummyZoraLinkModel = mpDummyZoraFaceModel = mpDummyZoraHatModel = nullptr;
    mpDummyZoraHandModel = mpDummyZoraWoodSwordModel = nullptr;
    mpDummyMagicLinkModel = mpDummyMagicFaceModel = mpDummyMagicHatModel = nullptr;
    mpDummyMagicHandModel = mpDummyMagicWoodSwordModel = nullptr;
    mpDummyCasualLinkModel = mpDummyCasualFaceModel = mpDummyCasualHatModel = nullptr;
    mpDummyCasualHandModel = mpDummyCasualWoodSwordModel = nullptr;
    for (int i = 0; i < 2; i++) {
        mpDummyKokiriBootModels[i] = mpDummyZoraBootModels[i] = nullptr;
        mpDummyMagicBootModels[i] = mpDummyCasualBootModels[i] = nullptr;
    }
    mpDummyBoomerangModel = nullptr;
    for (int i = 0; i < 3; i++) {
        mpDummyMagicArmorBodyBrk[i] = nullptr;
        mpDummyMagicArmorHeadBrk[i] = nullptr;
    }
    mpDummyHylianShieldModel = mpDummyOrdonShieldModel = mpDummyWoodShieldModel = nullptr;
    mpDummyArrowAmmoModel = mpDummyBombArrowAmmoModel = mpDummySlingAmmoModel = nullptr;
    mpDummyLoadedAmmoModel = nullptr;
    mpDummyWolfModel = nullptr;
    for (J3DModel*& chain : mpDummyWolfChainModels) {
        chain = nullptr;
    }
    mDummyMidna.clearPointers();
    mDummyItemFx.clearPointers();
    for (twili::RecolorSet& set : mDummyRecolor) {
        set.reset();
    }
}

int daDummyPlayer_c::createHeapImpl() {
    clearPrivateModelPointers();
    if (!daAlink_c::createHeap()) {
        TwiliLog.warn("[dummy {}] createHeap: base Link heap setup failed",
                     mDummyClientId);
        return FALSE;
    }

    mpDummyKmdlArchive = mDummyArchives.get(kArcKmdl);
    if (!mpDummyKmdlArchive) {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kLinkArcPath);
        return FALSE;
    }

    mpDummyKokiriLinkModel = makeModel(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_e);
    mpDummyKokiriFaceModel = makeModelEx(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_FACE_e,
                                         0x80000, 0x20200);
    mpDummyKokiriHatModel = makeModel(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_HEAD_e);
    mpDummyKokiriHandModel = makeModel(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_HANDS_e);
    mpDummyKokiriWoodSwordModel = makeModel(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_SWB_e);
    for (J3DModel*& boot : mpDummyKokiriBootModels) {
        boot = makeModel(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_BOOTSH_e);
    }
    mpKanteraModel = makeModelEx(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_AL_KANTERA_e, 0, 0);
    if (mpKanteraModel) {
        mpKanteraModel->setUserArea(reinterpret_cast<uintptr_t>(this));
        mpKanteraModel->getModelData()->getJointNodePointer(1)->setCallBack(
            daDummyPlayer_kandelaarModelCallBack);
    }
    mpKanteraGlowModel =
        makeModelEx(mpDummyKmdlArchive, dRes_ID_KMDL_BMD_EF_KTGLOW_e, 0, 0x200);
    mpKanteraGlowBtk = loadPrivateBtk(mpDummyKmdlArchive, dRes_ID_KMDL_BTK_EF_KTGLOW_e);
    bindTexMtxAnm(mpKanteraGlowModel, mpKanteraGlowBtk);
    if (!mpDummyKokiriLinkModel || !mpDummyKokiriFaceModel || !mpDummyKokiriHatModel ||
        !mpDummyKokiriHandModel)
    {
        TwiliLog.warn("[dummy {}] createHeap: private Link model create failed",
                     mDummyClientId);
        return FALSE;
    }
    clearEyeTextureMaxLod(mpDummyKokiriFaceModel);

    mpDummyMmdlArchive = mDummyArchives.get(kArcMmdl);
    if (mpDummyMmdlArchive) {
        mpDummyMagicLinkModel = makeModelEx(mpDummyMmdlArchive, kMmdlBmdMl, 0x1000000, 0);
        mpDummyMagicFaceModel = makeModelEx(mpDummyMmdlArchive, kMmdlBmdAlFace,
                                           0x80000, 0x20200);
        mpDummyMagicHatModel = makeModelEx(mpDummyMmdlArchive, kMmdlBmdMlHead, 0x1000000, 0);
        mpDummyMagicHandModel = makeModel(mpDummyMmdlArchive, kMmdlBmdAlHands);
        mpDummyMagicWoodSwordModel = makeModel(mpDummyMmdlArchive, kMmdlBmdAlSwb);
        for (J3DModel*& boot : mpDummyMagicBootModels) {
            boot = makeModel(mpDummyMmdlArchive, kMmdlBmdAlBootsH);
        }
        clearEyeTextureMaxLod(mpDummyMagicFaceModel);
        mpDummyMagicArmorBodyBrk[0] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlBodyPowerDown);
        mpDummyMagicArmorBodyBrk[1] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlBodyPowerUpA);
        mpDummyMagicArmorBodyBrk[2] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlBodyPowerUpB);
        mpDummyMagicArmorHeadBrk[0] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlHeadPowerDown);
        mpDummyMagicArmorHeadBrk[1] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlHeadPowerUpA);
        mpDummyMagicArmorHeadBrk[2] =
            loadPrivateBrk(mpDummyMmdlArchive, kMmdlBrkMlHeadPowerUpB);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kMagicArmorArcPath);
    }

    mpDummyZmdlArchive = mDummyArchives.get(kArcZmdl);
    if (mpDummyZmdlArchive) {
        mpDummyZoraLinkModel = makeModel(mpDummyZmdlArchive, kZmdlBmdZl);
        mpDummyZoraFaceModel = makeModelEx(mpDummyZmdlArchive, kZmdlBmdZlFace,
                                          0x80000, 0x20200);
        mpDummyZoraHatModel = makeModel(mpDummyZmdlArchive, kZmdlBmdZlHead);
        mpDummyZoraHandModel = makeModel(mpDummyZmdlArchive, kZmdlBmdAlHands);
        mpDummyZoraWoodSwordModel = makeModel(mpDummyZmdlArchive, kZmdlBmdAlSwb);
        for (J3DModel*& boot : mpDummyZoraBootModels) {
            boot = makeModel(mpDummyZmdlArchive, kZmdlBmdAlBootsH);
        }
        clearEyeTextureMaxLod(mpDummyZoraFaceModel);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kZoraArcPath);
    }

    // The Ordon clothes.
    mpDummyBmdlArchive = mDummyArchives.get(kArcBmdl);
    if (mpDummyBmdlArchive) {
        mpDummyCasualLinkModel = makeModel(mpDummyBmdlArchive, kBmdlBmdBl);
        mpDummyCasualFaceModel = makeModelEx(mpDummyBmdlArchive, kBmdlBmdAlFace,
                                             0x80000, 0x20200);
        mpDummyCasualHatModel = makeModel(mpDummyBmdlArchive, kBmdlBmdBlHead);
        mpDummyCasualHandModel = makeModel(mpDummyBmdlArchive, kBmdlBmdBlHands);
        mpDummyCasualWoodSwordModel = makeModel(mpDummyBmdlArchive, kBmdlBmdAlSwb);
        for (J3DModel*& boot : mpDummyCasualBootModels) {
            boot = makeModel(mpDummyBmdlArchive, kBmdlBmdAlBootsH);
        }
        clearEyeTextureMaxLod(mpDummyCasualFaceModel);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kCasualArcPath);
    }

    mpLinkModel = mpDummyKokiriLinkModel;
    mpLinkFaceModel = mpDummyKokiriFaceModel;
    mpLinkHatModel = mpDummyKokiriHatModel;
    mpLinkHandModel = mpDummyKokiriHandModel;
    mWoodSwordModel = mpDummyKokiriWoodSwordModel;

    mpDummyAlinkArchive = mDummyArchives.get(kArcAlink);
    if (!mpDummyAlinkArchive) {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kAlinkArcPath);
        return FALSE;
    }

    mpSwAModel = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_SWA_e, 0x80000, 0x200);
    mpSwASheathModel = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_PODA_e);
    mpSwMModel = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_SWM_e, 0, 0x1000200);
    mpSwMSheathModel = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_PODM_e, 0, 0);
    mpDummyArrowAmmoModel = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_ARROW_e);
    mpDummyBombArrowAmmoModel = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_ARROWB_e);
    mpDummySlingAmmoModel = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_PACHI_NUT_e);
    // The boomerang in hand.
    mpDummyBoomerangModel = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_BOOM_e);
    {
        twili::DummyItemFx::Models items;
        for (J3DModel*& m : items.arrow) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_ARROW_e);
        }
        for (J3DModel*& m : items.bombArrow) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_ARROWB_e);
        }
        for (J3DModel*& m : items.seed) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_PACHI_NUT_e);
        }
        items.boomerang = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_BOOM_e);
        items.tornado = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_EF_SHIPPU_e, 0, 0x200);
        items.tornadoBck = loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_EF_SHIPPU_e);
        items.tornadoBtk = loadPrivateBtk(mpDummyAlinkArchive, dRes_ID_ALINK_BTK_EF_SHIPPU_e);
        items.boomGlow = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_EF_SETBOOM_e, 0, 0x200);
        items.boomGlowBtk = loadPrivateBtk(mpDummyAlinkArchive, dRes_ID_ALINK_BTK_EF_SETBOOM_e);
        for (J3DModel*& m : items.bomb) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_BOMB_e);
        }
        for (J3DModel*& m : items.waterBomb) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_PG_e);
        }
        for (J3DModel*& m : items.insectBomb) {
            m = makeModel(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_PB_e);
        }
        items.insectMoveBck = loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_PB_MOVE_e);
        items.insectWaitBck = loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_PB_WAIT_e);
        // daSpinner_c::createHeap's and daCrod_c::createHeap's flags.
        items.spinner = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_AL_SP_e, 0x80000,
                                    0x11000084);
        items.spinnerBck = loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_SPOUT_e);
        items.crodBall = makeModelEx(mpDummyAlinkArchive, dRes_ID_ALINK_BMD_CROD_BALL_e, 0x80000,
                                     0x11000284);
        items.crodBrk = loadPrivateBrk(mpDummyAlinkArchive, dRes_ID_ALINK_BRK_CROD_BALL_e);
        items.crodBtk = loadPrivateBtk(mpDummyAlinkArchive, dRes_ID_ALINK_BTK_CROD_BALL_e);
        items.crodWaitBck =
            loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_CROD_BALL_WAIT_A_e);
        items.crodAimBck =
            loadPrivateBck(mpDummyAlinkArchive, dRes_ID_ALINK_BCK_CROD_BALL_WAIT_A_T_e);
        mDummyItemFx.createHeap(items);
    }
    if (!mpSwAModel || !mpSwASheathModel || !mpSwMModel || !mpSwMSheathModel) {
        TwiliLog.warn("[dummy {}] createHeap: private sword model create failed",
                     mDummyClientId);
        return FALSE;
    }

    m_nSwordBtk = loadPrivateBtk(mpDummyAlinkArchive, dRes_ID_ALINK_BTK_AL_SWA_e);
    bindTexMtxAnm(mpSwAModel, m_nSwordBtk);
    m_mSwordBtk = loadPrivateBtk(mpDummyAlinkArchive, dRes_ID_ALINK_BTK_AL_SWM_e);
    bindTexMtxAnm(mpSwMModel, m_mSwordBtk);
    m_mSwordBrk = loadPrivateBrk(mpDummyAlinkArchive, dRes_ID_ALINK_BRK_AL_SWM_e);
    bindTevRegAnm(mpSwMModel, m_mSwordBrk);

    mpDummyHylianShieldArchive = mDummyArchives.get(kArcHyShd);
    if (mpDummyHylianShieldArchive) {
        mpDummyHylianShieldModel =
            makeModel(mpDummyHylianShieldArchive, dRes_ID_HYSHD_BMD_AL_SHA_e);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kHylianShieldArcPath);
    }

    mpDummyOrdonShieldArchive = mDummyArchives.get(kArcCWShd);
    if (mpDummyOrdonShieldArchive) {
        mpDummyOrdonShieldModel =
            makeModel(mpDummyOrdonShieldArchive, dRes_ID_CWSHD_BMD_AL_SHB_e);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kOrdonShieldArcPath);
    }

    mpDummyWoodShieldArchive = mDummyArchives.get(kArcSWShd);
    if (mpDummyWoodShieldArchive) {
        mpDummyWoodShieldModel =
            makeModel(mpDummyWoodShieldArchive, dRes_ID_SWSHD_BMD_AL_SHC_e);
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kWoodShieldArcPath);
    }

    // The player's own Wmdl is only resident while it is a wolf itself (loadModelDVD).
    mpDummyWmdlArchive = mDummyArchives.get(kArcWmdl);
    if (mpDummyWmdlArchive) {
        // Flags as changeWolf.
        mpDummyWolfModel =
            makeModelEx(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_WL_e, 0x80000, 0x20200);
        bool chainsOk = true;
        for (J3DModel*& chain : mpDummyWolfChainModels) {
            chain = makeModel(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_WL_KUSARI_e);
            chainsOk &= chain != nullptr;
        }
        if (mpDummyWolfModel && chainsOk) {
            clearTextureMaxLod(mpDummyWolfModel, "wl_eyeball");
            // Midna rides on this body
            const twili::DummyMidna::Models midna = {
                loadPrivateBmd(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_MD_e),
                makeModelEx(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_MD_MASK_e, 0x80000, 0x1000000),
                makeModelEx(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_MD_HANDS_e, 0x80000, 0x1000000),
                makeModelEx(mpDummyWmdlArchive, dRes_ID_WMDL_BMD_MD_HAIR_HAND_e, 0, 0x1000000),
                {
                    loadPrivateBrk(mpDummyWmdlArchive, dRes_ID_WMDL_BRK_MD_D_COLOR_e),
                    loadPrivateBrk(mpDummyWmdlArchive, dRes_ID_WMDL_BRK_MD_MASK_D_COLOR_e),
                    loadPrivateBrk(mpDummyWmdlArchive, dRes_ID_WMDL_BRK_MD_HANDS_D_COLOR_e),
                    loadPrivateBrk(mpDummyWmdlArchive, dRes_ID_WMDL_BRK_MD_HAIR_HAND_D_COLOR_e),
                },
            };
            if (mDummyMidna.createHeap(midna)) {
                clearTextureMaxLod(mDummyMidna.bodyModel(), "midona_eyeball");
            } else {
                mDummyMidna.clearPointers();
                TwiliLog.warn("[dummy {}] createHeap: private Midna models failed",
                             mDummyClientId);
            }
        } else {
            TwiliLog.warn("[dummy {}] createHeap: private wolf model create failed",
                         mDummyClientId);
            mpDummyWolfModel = nullptr;
        }
    } else {
        TwiliLog.warn("[dummy {}] createHeap: private {} mount failed",
                     mDummyClientId, kWolfArcPath);
    }

    bindRemoteRecolor();

    // fopAcM_entrySolidHeap trims the heap to this use right after
    JKRHeap* heap = mDoExt_getCurrentHeap();
    const u32 heapSize = heap->getHeapSize();
    const u32 heapUsed = heapSize - heap->getFreeSize();
    mDummyHeapUsed = heapUsed;
    TwiliLog.info("[dummy {}] createHeap OK, heap used 0x{:X} of 0x{:X}",
                 mDummyClientId, heapUsed, heapSize);
    if (heapUsed > heapSize / 4 * 3) {
        TwiliLog.warn("[dummy {}] createHeap: heap over three quarters full",
                     mDummyClientId);
    }
    return TRUE;
}

int daDummyPlayer_c::initializeShell(const LinkPuppetState& state) {
    ScopedSelectEquip equip(state.clothesItem, state.swordItem, state.shieldItem);

    setArcName(FALSE);
    setShieldArcName();

    mAnmHeap3.setBufferSize(0x20000);
    PLAYER_CREATE_ANM_HEAP(mAnmHeap3, daPy_anmHeap_c::HEAP_TYPE_4,
                           "daDummyPlayer_c::mAnmHeap3");

    mSwordModel = mpSwAModel;
    mSheathModel = mpSwASheathModel;
    mShieldModel = mpDummyOrdonShieldModel ? mpDummyOrdonShieldModel : mpDummyHylianShieldModel;

    mAnmHeap4.setBufferSize(0xB00);
    PLAYER_CREATE_ANM_HEAP(mAnmHeap4, daPy_anmHeap_c::HEAP_TYPE_4,
                           "daDummyPlayer_c::mAnmHeap4");

    mCcStts.Init(120, 0xFF, this);
    initRemoteHurtbox();
    field_0x2e44.field_0xc = 0;
    field_0x306c = shape_angle.y + mBodyAngle.y;
    mHeight = 180.0f;
    mMaxSpeed = mpHIO->mMove.m.mMaxSpeed;

    mLinkAcch.Set(this, 3, mAcchCir);
    mLinkAcch.ClrWaterNone();
    mLinkAcch.SetWaterCheckOffset(10000.0f);
    mLinkAcch.OnLineCheck();
    mLinkAcch.ClrRoofNone();
    mLinkAcch.SetRoofCrrHeight(mHeight);
    mLinkAcch.SetGndThinCellingOff();
    mLinkAcch.SetWtrChkMode(2);
    mLinkAcch.OnWallSort();
    mAcchCir[0].SetWall(30.0f, 35.0f);
    mAcchCir[1].SetWall(129.99000549316406f, 35.0f);
    mAcchCir[2].SetWall(mHeight, 35.0f);

    for (int i = 0; i < kAnmPackCount; i++) {
        mUnderAnmHeap[i].setBuffer(mUnderAnmHeap[0].getBuffer() + (i * kAnmPackBufferSize));
        PLAYER_CREATE_ANM_HEAP_F(mUnderAnmHeap[i], daPy_anmHeap_c::HEAP_TYPE_3,
                                 "daDummyPlayer_c::mUnderAnmHeap[%d]", i);
    }
    mUpperAnmHeap[0].setBuffer(mUnderAnmHeap[0].getBuffer() + 0x8400);
    for (int i = 0; i < kAnmPackCount; i++) {
        mUpperAnmHeap[i].setBuffer(mUpperAnmHeap[0].getBuffer() + (i * kAnmPackBufferSize));
        PLAYER_CREATE_ANM_HEAP_F(mUpperAnmHeap[i], daPy_anmHeap_c::HEAP_TYPE_3,
                                 "daDummyPlayer_c::mUpperAnmHeap[%d]", i);
    }

    PLAYER_CREATE_ANM_HEAP(mFaceBtpHeap, daPy_anmHeap_c::HEAP_TYPE_1,
                           "daDummyPlayer_c::mFaceBtpHeap");
    PLAYER_CREATE_ANM_HEAP(mFaceBtkHeap, daPy_anmHeap_c::HEAP_TYPE_2,
                           "daDummyPlayer_c::mFaceBtkHeap");
    PLAYER_CREATE_ANM_HEAP(mFaceBckHeap, daPy_anmHeap_c::HEAP_TYPE_3,
                           "daDummyPlayer_c::mFaceBckHeap");
    J3DAnmBase* faceBtp =
        static_cast<J3DAnmBase*>(mFaceBtpHeap.loadDataIdx(dRes_ID_ALANM_BTP_FMABA01_e));
    J3DAnmBase* faceBtk =
        static_cast<J3DAnmBase*>(mFaceBtkHeap.loadDataIdx(dRes_ID_ALANM_BTK_FMABA01_e));
    J3DAnmBase* faceBck =
        static_cast<J3DAnmBase*>(mFaceBckHeap.loadDataIdx(dRes_ID_ALANM_BCK_FA_e));
    mpFaceBtp = faceBtp && faceBtp->getKind() == 2 ? static_cast<J3DAnmTexPattern*>(faceBtp)
                                                   : nullptr;
    mpFaceBtk = faceBtk && faceBtk->getKind() == 4 ? static_cast<J3DAnmTextureSRTKey*>(faceBtk)
                                                   : nullptr;
    // mFaceBck holds the FAT clip createHeap built, whose data that read just overwrote.
    mDummyFaceBckReady = isTransformAnm(faceBck);
    if (mDummyFaceBckReady) {
        J3DAnmTransform* bck = static_cast<J3DAnmTransform*>(faceBck);
        mFaceBck.init(bck, FALSE, bck->getAttribute(), 1.0f, 0, -1, true);
    }
    mDummyMidna.initHeaps();
    for (int i = 0; i < 2; i++) {
        mItemHeap[i].setBufferSize(0x13200);
        PLAYER_CREATE_ANM_HEAP_F(mItemHeap[i], daPy_anmHeap_c::HEAP_TYPE_4,
                                 "daDummyPlayer_c::mItemHeap[%d]", i);
    }
    PLAYER_CREATE_ANM_HEAP(mAnmHeap9, daPy_anmHeap_c::HEAP_TYPE_3,
                           "daDummyPlayer_c::mAnmHeap9");
    resetBasAnime();

    mZ2Link.init(&current.pos, &eyePos, &field_0x3720);
    mZ2Link.initKantera(&mKandelaarFlamePos);
    mZ2Link.setKanteraState(0);
    mZ2Link.setLinkState(0);
    restoreLocalLinkAudioPtr();
    mProcID = PROC_MAX;

    m_swordBlur.m_blurTex =
        static_cast<ResTIMG*>(mpDummyAlinkArchive->getIdxResource(dRes_ID_ALINK_BTI_BLUR_e));
    m_swordBlur.field_0x14 = 0;
    m_swordBlur.field_0x20 = 0;

    mWaterY = -G_CM3D_F_INF;
    field_0x33b8 = -G_CM3D_F_INF;
    field_0x33bc = -G_CM3D_F_INF;
    mGroundCode = -1;
    mGndPolyAtt0 = 16;
    mGndPolyAtt1 = 0;
    mGndPolySpecialCode = dBgW_SPCODE_NORMAL;
    mEquipItem = dItemNo_NONE_e;
    mAlinkStaffId = -1;
    mExitID = 0x3F;
    onNoResetFlg0(FLG0_SWIM_UP);
    onNoResetFlg2(FLG2_PLAYER_SHADOW_NO_DRAW);
    offOxygenTimer();

    field_0x2f94 = 0xFF;
    field_0x2f95 = 0xFF;
    field_0x2f96 = 0xFF;
    field_0x2f97 = 0xFF;
    mAtnActorID = fpcM_ERROR_PROCESS_ID_e;
    mMsgClassID = fpcM_ERROR_PROCESS_ID_e;
    field_0x28f8 = fpcM_ERROR_PROCESS_ID_e;
    field_0x28fc = fpcM_ERROR_PROCESS_ID_e;
    field_0x317c = dComIfGp_getPlayerCameraID(0);
    field_0x2e54.init(&mLinkAcch, mpHIO->mBasic.m.mWaterSurfaceEffectHeight, mHeight);

    setRemoteBodyMetrics(false);
    mFallVoiceInit = 0;

    applyRemoteClothes(state.clothesItem);

    if (!applyRemoteState(state)) {
        return FALSE;
    }

    mDummyShellReady = true;
    return TRUE;
}

// As setMagicArmorBrk, from the dummy's private Mmdl.
void daDummyPlayer_c::bindRemoteMagicArmorBrk(int status, bool fromStart) {
    status = (std::max)(0, (std::min)(status, 2));
    mMagicArmorBodyBrk = mpDummyMagicArmorBodyBrk[status];
    mMagicArmorHeadBrk = mpDummyMagicArmorHeadBrk[status];
    mDummyArmorBrkStatus = static_cast<int8_t>(status);

    if (mpLinkModel && mMagicArmorBodyBrk) {
        J3DModelData* bodyData = mpLinkModel->getModelData();
        mMagicArmorBodyBrk->searchUpdateMaterialID(bodyData);
        bodyData->entryTevRegAnimator(mMagicArmorBodyBrk);
        mMagicArmorBodyBrk->setFrame(fromStart ? 0.0f : mMagicArmorBodyBrk->getFrameMax());
    }

    if (mpLinkHatModel && mMagicArmorHeadBrk) {
        J3DModelData* headData = mpLinkHatModel->getModelData();
        mMagicArmorHeadBrk->searchUpdateMaterialID(headData);
        headData->entryTevRegAnimator(mMagicArmorHeadBrk);
        mMagicArmorHeadBrk->setFrame(fromStart ? 0.0f : mMagicArmorHeadBrk->getFrameMax());
    }
}

// The remote's Magic Armor power (kStatusArmorDrained), not our rupee count
void daDummyPlayer_c::updateRemoteMagicArmor(const LinkPuppetState& state) {
    const bool worn = !checkWolf() && mDummyClothesItem == dItemNo_ARMOR_e && mMagicArmorBodyBrk;
    const bool wasWorn = mDummyArmorWorn;
    mDummyArmorWorn = worn;
    if (!worn) {
        return;
    }
    const int status = (state.status.flags & twili::kStatusArmorDrained) ? 0 : 1;
    if (!wasWorn || status != mDummyArmorBrkStatus) {
        // Just put on (applyRemoteClothes bound power_up_a)
        bindRemoteMagicArmorBrk(status, wasWorn);
    }
    for (J3DAnmTevRegKey* brk : {mMagicArmorBodyBrk, mMagicArmorHeadBrk}) {
        if (brk != nullptr) {
            brk->setFrame((std::min)(brk->getFrame() + 1.0f, static_cast<f32>(brk->getFrameMax())));
        }
    }
}

void daDummyPlayer_c::applyRemoteClothes(uint8_t clothesItem) {
    clothesItem = normalizeClothesItem(clothesItem);
    const bool useMagicArmor =
        clothesItem == dItemNo_ARMOR_e &&
        mpDummyMagicLinkModel && mpDummyMagicFaceModel &&
        mpDummyMagicHatModel && mpDummyMagicHandModel &&
        mpDummyMagicArmorBodyBrk[1] && mpDummyMagicArmorHeadBrk[1];
    const bool useZora =
        clothesItem == dItemNo_WEAR_ZORA_e &&
        mpDummyZoraLinkModel && mpDummyZoraFaceModel &&
        mpDummyZoraHatModel && mpDummyZoraHandModel;
    const bool useCasual =
        clothesItem == dItemNo_WEAR_CASUAL_e &&
        mpDummyCasualLinkModel && mpDummyCasualFaceModel &&
        mpDummyCasualHatModel && mpDummyCasualHandModel;

    J3DModel* bodyModel = mpDummyKokiriLinkModel;
    J3DModel* faceModel = mpDummyKokiriFaceModel;
    J3DModel* hatModel = mpDummyKokiriHatModel;
    J3DModel* handModel = mpDummyKokiriHandModel;
    J3DModel* woodSwordModel = mpDummyKokiriWoodSwordModel;
    J3DModel* const* bootModels = mpDummyKokiriBootModels;
    uint8_t activeClothes = dItemNo_WEAR_KOKIRI_e;
    // mZ2Link's state for each set, as changeLink sets it.
    uint8_t linkSoundState = 0;
    if (useMagicArmor) {
        bodyModel = mpDummyMagicLinkModel;
        faceModel = mpDummyMagicFaceModel;
        hatModel = mpDummyMagicHatModel;
        handModel = mpDummyMagicHandModel;
        woodSwordModel = mpDummyMagicWoodSwordModel ? mpDummyMagicWoodSwordModel : woodSwordModel;
        bootModels = mpDummyMagicBootModels;
        activeClothes = dItemNo_ARMOR_e;
        linkSoundState = 4;
    } else if (useZora) {
        bodyModel = mpDummyZoraLinkModel;
        faceModel = mpDummyZoraFaceModel;
        hatModel = mpDummyZoraHatModel;
        handModel = mpDummyZoraHandModel;
        woodSwordModel = mpDummyZoraWoodSwordModel ? mpDummyZoraWoodSwordModel : woodSwordModel;
        bootModels = mpDummyZoraBootModels;
        activeClothes = dItemNo_WEAR_ZORA_e;
        linkSoundState = 3;
    } else if (useCasual) {
        bodyModel = mpDummyCasualLinkModel;
        faceModel = mpDummyCasualFaceModel;
        hatModel = mpDummyCasualHatModel;
        handModel = mpDummyCasualHandModel;
        woodSwordModel =
            mpDummyCasualWoodSwordModel ? mpDummyCasualWoodSwordModel : woodSwordModel;
        bootModels = mpDummyCasualBootModels;
        activeClothes = dItemNo_WEAR_CASUAL_e;
        linkSoundState = 2;
    }

    if (!bodyModel || !faceModel || !hatModel || !handModel) {
        return;
    }

    if (mDummyClothesItem == activeClothes && mpLinkModel == bodyModel &&
        mpLinkFaceModel == faceModel && mpLinkHatModel == hatModel &&
        mpLinkHandModel == handModel)
    {
        return;
    }

    mpLinkModel = bodyModel;
    mpLinkFaceModel = faceModel;
    mpLinkHatModel = hatModel;
    mpLinkHandModel = handModel;
    mWoodSwordModel = woodSwordModel ? woodSwordModel : mpDummyKokiriWoodSwordModel;
    mDummyClothesItem = activeClothes;
    // draw() draws the boots whenever the flag is set
    mpLinkBootModels[0] = bootModels[0];
    mpLinkBootModels[1] = bootModels[1];
    if (!mpLinkBootModels[0] || !mpLinkBootModels[1]) {
        offNoResetFlg0(FLG0_EQUIP_HVY_BOOTS);
    }

    // The casual head has 6 joints
    if (useCasual) {
        onNoResetFlg2(FLG2_UNK_100000);
    } else {
        offNoResetFlg2(FLG2_UNK_100000);
    }

    mpLinkModel->setUserArea(reinterpret_cast<uintptr_t>(this));
    mpLinkHatModel->setUserArea(reinterpret_cast<uintptr_t>(this));
    changeModelDataDirect(0);

    field_0x06e4 = nullptr;
    if (useMagicArmor) {
        field_0x06d8 = getMaterialShape(mpLinkModel, 4);
        field_0x06dc = getMaterialShape(mpLinkModel, 5);
        field_0x06e0 = getMaterialShape(mpLinkModel, 10);
        field_0x06e8 = getMaterialShape(mpLinkModel, 3);
        field_0x06ec = getMaterialShape(mpLinkModel, 1);
        field_0x06f0 = getMaterialShape(mpLinkModel, 2);
    } else if (useZora) {
        field_0x06d8 = getMaterialShape(mpLinkModel, 4);
        field_0x06dc = getMaterialShape(mpLinkModel, 5);
        field_0x06e0 = getMaterialShape(mpLinkModel, 8);
        field_0x06e4 = getMaterialShape(mpLinkModel, 9);
        field_0x06e8 = nullptr;
        field_0x06ec = getMaterialShape(mpLinkModel, 2);
        field_0x06f0 = getMaterialShape(mpLinkModel, 11);
        setShapeVisible(field_0x06e4, false);
    } else if (useCasual) {
        field_0x06d8 = getMaterialShape(mpLinkModel, 3);
        field_0x06dc = getMaterialShape(mpLinkModel, 4);
        field_0x06e0 = nullptr;
        field_0x06e8 = getMaterialShape(mpLinkModel, 2);
        field_0x06ec = getMaterialShape(mpLinkModel, 0);
        field_0x06f0 = getMaterialShape(mpLinkModel, 1);
    } else {
        setMaterialShapeVisible(mpLinkModel, 16, false);
        field_0x06d8 = getMaterialShape(mpLinkModel, 11);
        field_0x06dc = getMaterialShape(mpLinkModel, 12);
        field_0x06e0 = getMaterialShape(mpLinkModel, 6);
        field_0x06e8 = getMaterialShape(mpLinkModel, 8);
        field_0x06ec = getMaterialShape(mpLinkModel, 4);
        field_0x06f0 = getMaterialShape(mpLinkModel, 7);
    }

    mZ2Link.setLinkState(linkSoundState);
    restoreLocalLinkAudioPtr();

    if (!field_0x06d8) {
        field_0x06d8 = getMaterialShape(mpLinkModel, 0);
    }
    if (!field_0x06dc) {
        field_0x06dc = getMaterialShape(mpLinkModel, 0);
    }
    field_0x06d0 = field_0x06d8;
    field_0x06d4 = field_0x06dc;
    setShapeVisible(field_0x06e0, true);
    setShapeVisible(field_0x06e8, true);
    setShapeVisible(field_0x06ec, true);
    setShapeVisible(field_0x06f0, true);
    hideHandMaterials(mpLinkHandModel);

    if (J3DModelData* faceData = mpLinkFaceModel->getModelData()) {
        if (faceData->getMaterialNum() > 2) {
            faceData->getMaterialNodePointer(2)->setMaterialAnm(field_0x2180[0]);
        }
        if (faceData->getMaterialNum() > 3) {
            faceData->getMaterialNodePointer(3)->setMaterialAnm(field_0x2180[1]);
        }
        // Each set's face orders its materials its own way.
        if (mpFaceBtp) {
            mpFaceBtp->searchUpdateMaterialID(faceData);
            faceData->entryTexNoAnimator(mpFaceBtp);
        }
        if (mpFaceBtk) {
            mpFaceBtk->searchUpdateMaterialID(faceData);
            faceData->entryTexMtxAnimator(mpFaceBtk);
        }
    }

    if (useMagicArmor) {
        bindRemoteMagicArmorBrk(1);
    } else {
        mMagicArmorBodyBrk = nullptr;
        mMagicArmorHeadBrk = nullptr;
    }
}

J3DAnmTransform* daDummyPlayer_c::loadRemoteAnimationPack(bool upper, int idx, uint16_t anmId,
                                                          float frame, float frameNext,
                                                          float frameAlpha) {
    daPy_anmHeap_c& heap = upper ? mUpperAnmHeap[idx] : mUnderAnmHeap[idx];
    mDoExt_AnmRatioPack& pack =
        upper ? mNowAnmPackUpper[idx] : mNowAnmPackUnder[idx];
    uint16_t& active =
        upper ? mDummyUpperActiveAnm[idx] : mDummyLowerActiveAnm[idx];
    uint16_t& missing =
        upper ? mDummyUpperLastMissingAnm[idx] : mDummyLowerLastMissingAnm[idx];
    float& previousFrame = upper ? mDummyUpperPrevFrame[idx] : mDummyLowerPrevFrame[idx];
    bool& previousFrameValid =
        upper ? mDummyUpperPrevFrameValid[idx] : mDummyLowerPrevFrameValid[idx];
    uint16_t& refused = upper ? mDummyUpperRefusedAnm[idx] : mDummyLowerRefusedAnm[idx];
    const uint16_t oldActive = active;
    const bool basePack = !upper && idx == 0;
    // `heapIntact`
    auto unavailable = [&](bool heapIntact) -> J3DAnmTransform* {
        if (basePack) {
            return showBasePackFallback(heapIntact);
        }
        dropRemoteAnimationPack(upper, idx);
        return nullptr;
    };

    if (!isValidAnm(anmId)) {
        return unavailable(true);
    }

    J3DAnmTransform* bck = nullptr;
    // daAlink_c code the dummy runs can empty a pack behind our back
    if (anmId == active && pack.getAnmTransform() != nullptr) {
        bck = pack.getAnmTransform();
    } else {
        bool heapTouched = false;
        if (anmId != refused) {
            bck = loadPlayerBck(heap, anmId, packBufferSize(upper, idx), heapTouched);
            if (bck && !bckFitsBody(bck)) {
                TwiliLog.debug("[dummy {}] {} pack {} animation 0x{:X} has {} tracks, "
                              "the body {} joints", mDummyClientId, upper ? "upper" : "lower",
                              idx, anmId, bck->field_0x1e, bodyJointNum());
                refused = anmId;
                mDummyAnmBufferClobbered = true;
            }
        }
        if (anmId == refused) {
            mDummyRefusedAnmCount++;
            return unavailable(!heapTouched);
        }
        if (!bck) {
            if (missing != anmId) {
                TwiliLog.warn("[dummy {}] missing {} BCK pack {} animation 0x{:X}",
                             mDummyClientId, upper ? "upper" : "lower", idx, anmId);
                missing = anmId;
                mDummyMissingAnmCount++;
            }
            return unavailable(!heapTouched);
        }

        pack.setAnmTransform(bck);
        active = anmId;
    }
    if (basePack) {
        mDummyBaseHoldTicks = 0;
        mDummyBasePackFallback = false;
    }

    const float normalized =
        normalizeFrame(bck, blendRemoteFrame(bck, frame, frameNext, frameAlpha));
    float rate = 0.0f;
    if (previousFrameValid && oldActive == anmId) {
        rate = normalized - previousFrame;
        const s16 maxFrame = bck->getFrameMax();
        const float max = static_cast<float>(maxFrame);
        if (max > 0.0f &&
            (bck->getAttribute() == J3DFrameCtrl::EMode_LOOP ||
             bck->getAttribute() == J3DFrameCtrl::EMode_LOOP_REVERSE))
        {
            if (rate < -(max * 0.5f)) {
                rate += max;
            } else if (rate > max * 0.5f) {
                rate -= max;
            }
        } else if (max > 0.0f && std::fabs(rate) > max * 0.5f) {
            rate = 0.0f;
        }
    }
    previousFrame = normalized;
    previousFrameValid = true;

    const s16 endFrame = bck->getFrameMax();
    bck->setFrame(normalized);
    if (upper) {
        setFrameCtrl(&mUpperFrameCtrl[idx], bck->getAttribute(), 0, endFrame, rate, normalized);
    } else {
        setFrameCtrl(&mUnderFrameCtrl[idx], bck->getAttribute(), 0, endFrame, rate, normalized);
    }
    return bck;
}

bool daDummyPlayer_c::setRemoteBasAnime(bool upper, int idx) {
    if (idx < 0 || idx >= kAnmPackCount) {
        resetBasAnime();
        return false;
    }

    daPy_anmHeap_c& heap = upper ? mUpperAnmHeap[idx] : mUnderAnmHeap[idx];
    daPy_frameCtrl_c& frameCtrl = upper ? mUpperFrameCtrl[idx] : mUnderFrameCtrl[idx];
    const uint16_t active = upper ? mDummyUpperActiveAnm[idx] : mDummyLowerActiveAnm[idx];
    if (!isValidAnm(active)) {
        resetBasAnime();
        return false;
    }

    if (!heap.checkNoSetArcNo()) {
        resetBasAnime();
        return false;
    }

    JUTDataFileHeader* header = reinterpret_cast<JUTDataFileHeader*>(heap.getBuffer());
    if (!header || header->mSeAnmOffset == 0xFFFFFFFF) {
        resetBasAnime();
        return false;
    }

    if (field_0x3084 == heap.getIdx() && field_0x3086 == heap.getArcNo() &&
        field_0x33d4 * frameCtrl.getRate() >= 0.0f)
    {
        field_0x2d7c = &frameCtrl;
        return true;
    }

    const u32 seOffset = header->mSeAnmOffset;
    const u32 fileSize = header->mFileSize;
    if (!field_0x2d78 || seOffset >= fileSize) {
        resetBasAnime();
        return false;
    }

    const u32 dataSize = fileSize - seOffset;
    static constexpr u32 kBasAnmBufferSize = 0x800;
    if (dataSize >= kBasAnmBufferSize) {
        TwiliLog.warn("[dummy {}] {} BAS pack {} animation {} too large ({})",
                     mDummyClientId, upper ? "upper" : "lower", idx, active, dataSize);
        resetBasAnime();
        return false;
    }

    std::memcpy(field_0x2d78, heap.getBuffer() + seOffset, dataSize);
    field_0x2d7c = &frameCtrl;
    field_0x3084 = heap.getIdx();
    field_0x3086 = heap.getArcNo();
    field_0x33d4 = frameCtrl.getRate();
    field_0x2d80 = nullptr;

    ScopedDummyLinkAudioPtr scoped(&mZ2Link);
    initBasAnime();
    return true;
}

u16 daDummyPlayer_c::bodyJointNum() const {
    return mpLinkModel->getModelData()->getJointNum();
}

// A body clip has one track per joint (J3DAnmLoader).
bool daDummyPlayer_c::bckFitsBody(J3DAnmTransform* bck) const {
    return bck->field_0x1e == bodyJointNum();
}

// Empties one pack
void daDummyPlayer_c::dropRemoteAnimationPack(bool upper, int idx) {
    daPy_anmHeap_c& heap = upper ? mUpperAnmHeap[idx] : mUnderAnmHeap[idx];
    heap.resetIdx();
    heap.resetPriIdx();
    heap.resetArcNo();
    (upper ? mNowAnmPackUpper : mNowAnmPackUnder)[idx].setAnmTransform(nullptr);
    (upper ? mDummyUpperActiveAnm : mDummyLowerActiveAnm)[idx] = kNoAnm;
    (upper ? mDummyUpperPrevFrameValid : mDummyLowerPrevFrameValid)[idx] = false;
}

// Lower pack 0 is the pose every other pack blends onto, so it never goes empty.
J3DAnmTransform* daDummyPlayer_c::showBasePackFallback(bool heapIntact) {
    mDoExt_AnmRatioPack& pack = mNowAnmPackUnder[0];
    daPy_frameCtrl_c& frameCtrl = mUnderFrameCtrl[0];
    mDummyBasePackFallback = true;
    if (!heapIntact || !isValidAnm(mDummyLowerActiveAnm[0])) {
        dropRemoteAnimationPack(false, 0);
    }

    const u16 idleAnm = checkWolf() ? dRes_ID_ALANM_BCK_WL_WAITA_e : dRes_ID_ALANM_BCK_WAITS_e;
    J3DAnmTransform* held = pack.getAnmTransform();
    const bool idling = mDummyLowerActiveAnm[0] == idleAnm;
    if (held && (idling || mDummyBaseHoldTicks < kDummyBaseHoldTicks)) {
        if (!idling) {
            mDummyBaseHoldTicks++;
        }
        frameCtrl.updateFrame();
        held->setFrame(frameCtrl.getFrame());
        mDummyLowerPrevFrameValid[0] = false;
        return held;
    }

    bool heapTouched = false;
    J3DAnmTransform* idle =
        loadPlayerBck(mUnderAnmHeap[0], idleAnm, packBufferSize(false, 0), heapTouched);
    if (!idle || !bckFitsBody(idle)) {
        dropRemoteAnimationPack(false, 0);
        return nullptr;
    }
    pack.setAnmTransform(idle);
    mDummyLowerActiveAnm[0] = idleAnm;
    mDummyLowerPrevFrameValid[0] = false;
    setFrameCtrl(&frameCtrl, idle->getAttribute(), 0, idle->getFrameMax(), 1.0f, 0.0f);
    idle->setFrame(0.0f);
    return idle;
}

// Drops every loaded body clip
void daDummyPlayer_c::forgetRemoteAnimationPacks() {
    for (int i = 0; i < kAnmPackCount; i++) {
        dropRemoteAnimationPack(false, i);
        dropRemoteAnimationPack(true, i);
    }
}

bool daDummyPlayer_c::loadRemoteAnimationPacks(const LinkPuppetState& state,
                                               float lowerRatios[kAnmPackCount],
                                               float upperRatios[kAnmPackCount]) {
    bool haveLower = false;
    for (int i = 0; i < kAnmPackCount; i++) {
        J3DAnmTransform* bck =
            loadRemoteAnimationPack(false, i, state.lowerANMs[i], state.lowerFrames[i],
                                    state.lowerFramesNext[i], state.frameAlpha);
        lowerRatios[i] = bck ? clampRatio(state.lowerRatios[i]) : 0.0f;
        haveLower |= bck != nullptr;
    }

    // Pack 0 always holds a clip (showBasePackFallback) unless even the idle one failed to load.
    J3DAnmTransform* lowerBaseBck = mNowAnmPackUnder[0].getAnmTransform();
    if (!lowerBaseBck) {
        return false;
    }
    if (mDummyBasePackFallback) {
        lowerRatios[0] = (std::max)(lowerRatios[0], 1.0f - lowerRatios[1] - lowerRatios[2]);
    }

    bool lowerHasRatio = false;
    for (int i = 0; i < kAnmPackCount; i++) {
        lowerHasRatio |= lowerRatios[i] > 0.0f;
    }
    if (!lowerHasRatio) {
        lowerRatios[0] = 1.0f;
    }
    for (int i = 0; i < kAnmPackCount; i++) {
        if (!mNowAnmPackUnder[i].getAnmTransform()) {
            mNowAnmPackUnder[i].setAnmTransform(lowerBaseBck);
            lowerRatios[i] = 0.0f;
        }
        mNowAnmPackUnder[i].setRatio(lowerRatios[i]);
    }

    bool upperHasRatio = false;
    for (int i = 0; i < kAnmPackCount; i++) {
        J3DAnmTransform* bck =
            loadRemoteAnimationPack(true, i, state.upperANMs[i], state.upperFrames[i],
                                    state.upperFramesNext[i], state.frameAlpha);
        float ratio = bck ? clampRatio(state.upperRatios[i]) : 0.0f;

        if (!bck && i < 2 && mNowAnmPackUnder[i].getAnmTransform()) {
            mNowAnmPackUpper[i].setAnmTransform(mNowAnmPackUnder[i].getAnmTransform());
            ratio = clampRatio(state.upperRatios[i]);
            if (ratio <= 0.0f) {
                ratio = lowerRatios[i];
            }
        }

        mNowAnmPackUpper[i].setRatio(ratio);
        upperRatios[i] = ratio;
        upperHasRatio |= ratio > 0.0f && mNowAnmPackUpper[i].getAnmTransform();
    }

    for (int i = 0; i < kAnmPackCount; i++) {
        if (!mNowAnmPackUpper[i].getAnmTransform()) {
            J3DAnmTransform* fallback =
                mNowAnmPackUnder[i].getAnmTransform() ? mNowAnmPackUnder[i].getAnmTransform() :
                                                        lowerBaseBck;
            mNowAnmPackUpper[i].setAnmTransform(fallback);
            mNowAnmPackUpper[i].setRatio(0.0f);
        }
    }
    if (!upperHasRatio && mNowAnmPackUpper[0].getAnmTransform()) {
        mNowAnmPackUpper[0].setRatio(1.0f);
    }

    return haveLower && mNowAnmPackUnder[0].getAnmTransform() &&
           mNowAnmPackUpper[0].getAnmTransform();
}

bool daDummyPlayer_c::setRemoteAnimations(const LinkPuppetState& state) {
    float lowerRatios[kAnmPackCount] = {};
    float upperRatios[kAnmPackCount] = {};
    mDummyAnmBufferClobbered = false;
    bool loaded = loadRemoteAnimationPacks(state, lowerRatios, upperRatios);
    if (mDummyAnmBufferClobbered) {
        // The refused clip was read over whatever the packs loaded before it held.
        forgetRemoteAnimationPacks();
        loaded = loadRemoteAnimationPacks(state, lowerRatios, upperRatios);
    }
    if (!loaded) {
        return false;
    }

    field_0x2fb6 = state.upperBlendMode;
    field_0x3444 = clampRatio(state.upperBlendRatio);

    const int lowerDominantPack = dominantRemoteAnmPack(mDummyLowerActiveAnm, lowerRatios);
    const int upperDominantPack = dominantRemoteAnmPack(mDummyUpperActiveAnm, upperRatios);
    const uint16_t lowerDominant = dominantRemoteAnmId(mDummyLowerActiveAnm, lowerRatios);
    uint16_t upperDominant = dominantRemoteAnmId(mDummyUpperActiveAnm, upperRatios);
    if (upperDominant == kNoAnm) {
        upperDominant = lowerDominant;
    }

    const bool changedDominantAnm =
        (lowerDominant != kNoAnm && lowerDominant != mDummyLowerDominantAnm) ||
        (upperDominant != kNoAnm && upperDominant != mDummyUpperDominantAnm);
    if (mDummyAnimationBlendInitialized && changedDominantAnm && field_0x2060) {
        field_0x2060->initOldFrameMorf(kDummyRemoteMorfFrames, kDummyRemoteMorfStartJoint,
                                       bodyJointNum());
    }
    mDummyLowerDominantAnm = lowerDominant;
    mDummyUpperDominantAnm = upperDominant;
    mDummyLowerDominantPack = lowerDominantPack;
    mDummyUpperDominantPack = upperDominantPack;
    mDummyAnimationBlendInitialized = true;
    return true;
}

void daDummyPlayer_c::modelCalcRemoteBody(bool attentionLock) {
    const u8 oldRootTransMode = field_0x2f99;
    const cXyz oldRootTransBase = field_0x3588;
    const f32 oldRootTransBaseY = field_0x33b0;
    auto* const oldRideBase = field_0x384c;
    const s16 oldBodyAngleX = mBodyAngle.x;
    const u8 oldRideStatus = mRideStatus;
    const bool bowTilt = mDummyRider.active && mDummyRider.bowTilt && !checkWolf();

    field_0x2f99 = 5;
    if (mDummyRider.active && !checkWolf()) {
        field_0x2f99 = mDummyRider.rootMode;
        field_0x3588.x = mDummyRider.base[0];
        field_0x33b0 = mDummyRider.base[1];
        field_0x3588.z = mDummyRider.base[2];
        field_0x384c = const_cast<cXyz*>(&kDummyHorseBaseAnime);
        s16 bodyX = mDummyRider.pitchComp ? static_cast<s16>(mBodyAngle.x - shape_angle.x)
                                          : mBodyAngle.x;
        if (bowTilt) {
            // jointControll turns joint 5 for the bow only while riding, and then takes the
            // slope off joint 1 by its own rule: undone here so pitchComp alone decides.
            mRideStatus = RIDETYPE_HORSE;
            if (!checkHorseLieAnime() && mProcID != PROC_HORSE_RUN && mProcID != PROC_BOAR_RUN) {
                bodyX = static_cast<s16>(bodyX + shape_angle.x);
            }
        }
        mBodyAngle.x = bodyX;
    } else if (checkWolf()) {
        field_0x3588.x = kDummyWolfRootTransX;
        field_0x3588.z = kDummyWolfRootTransZ;
    } else {
        field_0x3588.x = attentionLock ? kDummyHalfAtnRootTransX : kDummyWaitRootTransX;
        field_0x3588.z = attentionLock ? kDummyHalfAtnRootTransZ : kDummyWaitRootTransZ;
    }
    modelCalc(mpLinkModel);
    if (bowTilt) {
        mDummyBowTiltTicks++;
        mDummyBowTiltDeg = jointExtraTurnDeg(5);
    }

    field_0x2f99 = oldRootTransMode;
    field_0x3588 = oldRootTransBase;
    field_0x33b0 = oldRootTransBaseY;
    field_0x384c = oldRideBase;
    mBodyAngle.x = oldBodyAngleX;
    mRideStatus = oldRideStatus;
}

// How far joint `jnt` is turned beyond its clips' pose (jointControll's extras), in degrees.
f32 daDummyPlayer_c::jointExtraTurnDeg(u16 jnt) const {
    J3DModelData* data = mpLinkModel->getModelData();
    const auto parentOf = [&](auto&& self, J3DJoint* joint, u16 child) -> J3DJoint* {
        for (; joint != nullptr; joint = joint->getYounger()) {
            for (J3DJoint* c = joint->getChild(); c != nullptr; c = c->getYounger()) {
                if (c->getJntNo() == child) {
                    return joint;
                }
            }
            if (J3DJoint* found = self(self, joint->getChild(), child)) {
                return found;
            }
        }
        return nullptr;
    };
    J3DJoint* parent = parentOf(parentOf, data->getJointNodePointer(0), jnt);
    if (parent == nullptr) {
        return 0.0f;
    }
    MtxP p = mpLinkModel->getAnmMtx(parent->getJntNo());
    MtxP c = mpLinkModel->getAnmMtx(jnt);
    Mtx anim;
    MTXQuat(anim, field_0x2060->getOldFrameQuaternion(jnt));
    // trace(Ranim^T * Rparent^T * Rchild), columns normalised against any joint scale
    f32 trace = 0.0f;
    for (int k = 0; k < 3; k++) {
        f32 local[3];
        f32 cl = 0.0f, pl[3] = {};
        for (int r = 0; r < 3; r++) {
            cl += c[r][k] * c[r][k];
        }
        for (int i = 0; i < 3; i++) {
            local[i] = 0.0f;
            for (int r = 0; r < 3; r++) {
                pl[i] += p[r][i] * p[r][i];
                local[i] += p[r][i] * c[r][k];
            }
        }
        for (int i = 0; i < 3; i++) {
            const f32 norm = std::sqrt(pl[i] * cl);
            trace += anim[i][k] * (norm > 1e-6f ? local[i] / norm : 0.0f);
        }
    }
    const f32 cosA = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(cosA) * 180.0f / 3.14159265f;
}

void daDummyPlayer_c::updateRemoteEquipment(const LinkPuppetState& state) {
    mDummySwordItem = normalizeSwordItem(state.swordItem);
    mDummyShieldItem = normalizeShieldItem(state.shieldItem);
    uint16_t equipItem = normalizeDummyEquipItem(state.equipItem, mDummySwordItem);
    if (equipItem != kSwordEquipItem &&
        shouldForceRemoteSwordInHand(state, mDummySwordItem))
    {
        equipItem = kSwordEquipItem;
    }
    mDummyGrassType = state.itemFx.hx.grassType;
    syncRemoteHeldItemModel(equipItem, state.itemBckId);
    if (isRemoteHeldItem(equipItem) && mDummyHeldItem != equipItem) {
        equipItem = dItemNo_NONE_e;
    }
    mEquipItem = equipItem;

    switch (mDummySwordItem) {
    case dItemNo_WOOD_STICK_e:
        mSwordModel = mWoodSwordModel ? mWoodSwordModel : mpSwAModel;
        mSheathModel = mpSwMSheathModel ? mpSwMSheathModel : mpSwASheathModel;
        break;
    case dItemNo_MASTER_SWORD_e:
    case dItemNo_LIGHT_SWORD_e:
        mSwordModel = mpSwMModel ? mpSwMModel : mpSwAModel;
        mSheathModel = mpSwMSheathModel ? mpSwMSheathModel : mpSwASheathModel;
        break;
    case dItemNo_SWORD_e:
    default:
        mSwordModel = mpSwAModel;
        mSheathModel = mpSwASheathModel;
        break;
    }

    const bool swordEquipped = mDummySwordItem != dItemNo_NONE_e;
    const bool swordInHand = mEquipItem == kSwordEquipItem;
    if (mDummySwordItem == dItemNo_WOOD_STICK_e) {
        setMaterialShapeVisible(mSwordModel, 1, swordEquipped && !swordInHand);
    } else {
        setMaterialShapeVisible(mSwordModel, 0, swordEquipped && swordInHand);
    }

    switch (mDummyShieldItem) {
    case dItemNo_HYLIA_SHIELD_e:
        mShieldModel = mpDummyHylianShieldModel;
        break;
    case dItemNo_SHIELD_e:
        mShieldModel = mpDummyWoodShieldModel;
        break;
    case dItemNo_WOOD_SHIELD_e:
    default:
        mShieldModel = mpDummyOrdonShieldModel;
        break;
    }

    if (!mShieldModel) {
        mShieldModel = mpDummyHylianShieldModel ? mpDummyHylianShieldModel :
                       (mpDummyOrdonShieldModel ? mpDummyOrdonShieldModel : mpDummyWoodShieldModel);
    }

    int swordType = 0;
    if (mDummySwordItem == dItemNo_LIGHT_SWORD_e) {
        swordType = 3;
    } else if (mDummySwordItem == dItemNo_MASTER_SWORD_e) {
        swordType = 2;
    } else if (swordEquipped) {
        swordType = 1;
    }

    const bool shieldEquipped = mDummyShieldItem != dItemNo_NONE_e;
    const int handState = swordInHand ? 1 : 2;
    const bool shieldInHandForAudio = state.shieldInHand || swordInHand;
    mZ2Link.setLinkSwordType(swordType, handState);
    mZ2Link.setLinkShieldType(shieldEquipped ? 1 : 0, shieldInHandForAudio ? 1 : 2);

    updateRemoteBodyShapes(state);
}

void daDummyPlayer_c::updateRemoteBodyShapes(const LinkPuppetState& state) {
    mDummyVisFlags = state.visFlags;

    // The sword belt, without a sword or with the wooden one.
    setShapeVisible(field_0x06ec,
                    mDummySwordItem != dItemNo_NONE_e && mDummySwordItem != dItemNo_WOOD_STICK_e);

    const bool heavy = (state.visFlags & twili::kVisHeavyBoots) != 0 &&
                       mpLinkBootModels[0] != nullptr && mpLinkBootModels[1] != nullptr;
    if (heavy != (checkEquipHeavyBoots() != 0)) {
        if (heavy) {
            onNoResetFlg0(FLG0_EQUIP_HVY_BOOTS);
        } else {
            offNoResetFlg0(FLG0_EQUIP_HVY_BOOTS);
        }
    }
    const int8_t bootsSoundType = heavy ? 1 : 0;
    if (bootsSoundType != mDummyBootsSoundType) {
        if (mDummyBootsSoundType < 0 && bootsSoundType == 0) {
            mDummyBootsSoundType = 0;  // Z2CreatureLink starts without boots
        } else if (mDummyShellReady && !isHidden() && isRemotePlayerAudioAudible(this)) {
            mZ2Link.setLinkBootsType(bootsSoundType);
            mDummyBootsSoundType = bootsSoundType;
        }
    }
    // The feet the boots go over.
    setShapeVisible(field_0x06e0, !heavy);

    if (mDummyClothesItem == dItemNo_WEAR_ZORA_e) {
        const bool mask = (state.visFlags & twili::kVisZoraMask) != 0;
        setShapeVisible(field_0x06f0, mask);
        setShapeVisible(field_0x06e4, mask && !heavy);
    }
}

// The lantern flag (FLG2_UNK_1) is updateRemoteLantern's
void daDummyPlayer_c::clearRemoteHeldItemModel() {
    if (!isRemoteHeldItem(mDummyHeldItem) && !mHeldItemModel) {
        return;
    }

    mEquipItem = isRemoteHeldItem(mDummyHeldItem) ? mDummyHeldItem : mEquipItem;
    deleteEquipItem(FALSE, TRUE);
    mDummyHeldItem = dItemNo_NONE_e;
}

void daDummyPlayer_c::syncRemoteHeldItemModel(uint16_t equipItem, uint16_t itemBckId) {
    if (!isRemoteHeldItem(equipItem)) {
        clearRemoteHeldItemModel();
        return;
    }

    if (equipItem == twili::wolffx::kLockDomeItem) {
        // Only a wolf holds it (a forged packet may say otherwise)
        if (!checkWolf()) {
            clearRemoteHeldItemModel();
            return;
        }
        if (mDummyHeldItem == equipItem && mHeldItemModel) {
            return;
        }
        clearRemoteHeldItemModel();
        if (loadRemoteLockDome()) {
            mDummyHeldItem = equipItem;
        } else {
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                TwiliLog.warn("[dummy {}] Midna's dome model unavailable", mDummyClientId);
            }
            mEquipItem = dItemNo_NONE_e;
            mDummyHeldItem = dItemNo_NONE_e;
        }
        return;
    }

    if (mDummyHeldItem == equipItem && (mHeldItemModel || equipItem == dItemNo_KANTERA_e) &&
        (equipItem != kMetamorphoseItem || mAnmHeap9.getIdx() == itemBckId) &&
        (equipItem != kGrassWhistleItem || mDummyHeldGrassType == mDummyGrassType))
    {
        return;
    }

    clearRemoteHeldItemModel();
    if (equipItem == dItemNo_KANTERA_e && (!mpKanteraModel || !mpKanteraGlowModel)) {
        TwiliLog.warn("[dummy {}] lantern item model unavailable", mDummyClientId);
        return;
    }

    mEquipItem = equipItem;
    if (equipItem == kMetamorphoseItem) {
        if (!setRemoteMetamorphoseModel(itemBckId)) {
            mHeldItemModel = nullptr;
        }
    } else if (equipItem == dItemNo_BOOMERANG_e) {
        mHeldItemModel = mpDummyBoomerangModel;
    } else if (daPy_py_c::checkBottleItem(equipItem)) {
        setBottleModel(equipItem);
        mDummyBottleBtk = 1;  // setBottleModel enters field_0x0718 last
    } else if (equipItem == dItemNo_IRONBALL_e) {
        // Not setItemModel
        setIronBallModel();
        // It sets mAtSph up as the ball's AT
        initRemoteHurtbox();
    } else if (equipItem == dItemNo_HORSE_FLUTE_e) {
        setHorseWhistleModel();
    } else if (equipItem == kGrassWhistleItem) {
        if (!loadRemoteGrassWhistle()) {
            mHeldItemModel = nullptr;
        }
    } else {
        setItemModel();
    }

    mAtCps[0].SetAtHitCallback(NULL);
    mAtCps[0].OffAtSetBit();

    if (mHeldItemModel || equipItem == dItemNo_KANTERA_e) {
        mDummyHeldItem = equipItem;
    } else {
        // setRemoteMetamorphoseModel said why
        if (equipItem != kMetamorphoseItem && equipItem != kGrassWhistleItem) {
            TwiliLog.warn("[dummy {}] unsupported held item model {}", mDummyClientId,
                         equipItem);
        }
        mEquipItem = dItemNo_NONE_e;
        mDummyHeldItem = dItemNo_NONE_e;
    }
}

bool daDummyPlayer_c::loadRemoteGrassWhistle() {
    static const char* const kArcNames[2] = {"J_Tobi", "J_Umak"};
    if (mDummyGrassType == 0) {
        return false;
    }
    const int type = mDummyGrassType - 1;
    dRes_info_c* info = dComIfG_getObjectResInfo(kArcNames[type]);
    if (info == nullptr || info->getArchive() == nullptr ||
        dComIfG_getObjectRes(kArcNames[type], 3) == nullptr)
    {
        return false;
    }
    setGrassWhistleModel(type);
    mDummyHeldGrassType = mDummyGrassType;
    return mHeldItemModel != nullptr;
}

bool daDummyPlayer_c::setRemoteMetamorphoseModel(uint16_t bckId) {
    if (bckId != dRes_ID_ALANM_BCK_WFCHANGEATOW_e && bckId != dRes_ID_ALANM_BCK_WFCHANGEWTOA_e) {
        static uint16_t sLoggedBck = kNoItemBck;
        if (bckId != sLoggedBck) {
            sLoggedBck = bckId;
            TwiliLog.warn("[dummy {}] transformation fur with item clip 0x{:X}",
                         mDummyClientId, bckId);
        }
        return false;
    }
    J3DAnmBase* anm = static_cast<J3DAnmBase*>(mAnmHeap9.loadDataIdx(bckId));
    if (!isTransformAnm(anm)) {
        mAnmHeap9.resetIdx();
        return false;
    }
    JKRHeap* heap = setItemHeap();
    // loadAramBmd reads a fresh copy of the model for every call, so no BMD is loaded twice.
    J3DModelData* data = loadAramBmd(dRes_ID_ALANM_BMD_AL_WF_e, 0x6000);
    mHeldItemModel = data != nullptr ? initModel(data, 0) : nullptr;
    const bool bound = mHeldItemModel != nullptr &&
                       mItemBck.init(static_cast<J3DAnmTransform*>(anm), FALSE, 2, 1.0f, 0, -1,
                                     false);
    mDoExt_setCurrentHeap(heap);
    if (!bound) {
        static bool sLogged = false;
        if (!sLogged) {
            sLogged = true;
            TwiliLog.warn("[dummy {}] transformation fur model unavailable", mDummyClientId);
        }
        mHeldItemModel = nullptr;
        mAnmHeap9.resetIdx();
        return false;
    }
    return true;
}

void daDummyPlayer_c::applyRemoteItemPresentation(const LinkPuppetState& state) {
    mLeftItemJntNo = sanitizeItemJoint(state.leftItemJoint, mpLinkModel, 10);
    mRightItemJntNo = sanitizeItemJoint(state.rightItemJoint, mpLinkModel, 15);
    field_0x33dc = sanitizeItemBckFrame(state.itemBckFrame);
    if (isRemoteBowItemBck(state.itemBckId, mEquipItem) &&
        mAnmHeap9.getIdx() != state.itemBckId)
    {
        changeItemBck(state.itemBckId, field_0x33dc);
    }
    field_0x301e = state.itemProjectileType == DUMMY_PROJECTILE_BOMB_ARROW ? 1 : 0;

    if (!isRemoteHeldItem(mEquipItem)) {
        field_0x2f94 = 0xFF;
        field_0x2f95 = 0xFF;
        field_0x2f96 = 0xFF;
        field_0x2f97 = 0xFF;
        return;
    }

    field_0x2f94 = sanitizeLeftItemHandOverride(state.leftHandItemOverride,
                                                mpLinkHandModel);
    field_0x2f95 = sanitizeRightItemHandOverride(state.rightHandItemOverride,
                                                 mpLinkHandModel);
    field_0x2f96 = sanitizeGripHandOverride(state.leftHandGripOverride,
                                            mpLinkHandModel);
    field_0x2f97 = sanitizeGripHandOverride(state.rightHandGripOverride,
                                             mpLinkHandModel);
}

J3DModel* daDummyPlayer_c::remoteAmmoModelForType(uint8_t type) const {
    switch (type) {
    case DUMMY_PROJECTILE_ARROW:
        return mpDummyArrowAmmoModel;
    case DUMMY_PROJECTILE_BOMB_ARROW:
        return mpDummyBombArrowAmmoModel;
    case DUMMY_PROJECTILE_SLING:
        return mpDummySlingAmmoModel;
    default:
        return nullptr;
    }
}

void daDummyPlayer_c::updateRemoteLoadedAmmo(const LinkPuppetState& state) {
    mpDummyLoadedAmmoModel = nullptr;
    if (!state.itemAmmoLoaded || !checkBowAndSlingItem(mEquipItem)) {
        return;
    }

    const uint8_t type =
        state.itemProjectileType != DUMMY_PROJECTILE_NONE ?
            state.itemProjectileType :
            (mEquipItem == dItemNo_PACHINKO_e ? DUMMY_PROJECTILE_SLING :
                                                DUMMY_PROJECTILE_ARROW);
    J3DModel* model = remoteAmmoModelForType(type);
    if (!model) {
        return;
    }

    if (type == DUMMY_PROJECTILE_SLING) {
        static const Vec slingLocalPos = {10.0f, 10.0f, 0.0f};
        mDoMtx_multVec(getLeftItemMatrix(), &slingLocalPos, &mHeldItemRootPos);
        mDoMtx_stack_c::transS(mHeldItemRootPos);
        mDoMtx_stack_c::ZXYrotM(mBodyAngle.x, shape_angle.y + mBodyAngle.y, 0);
    } else {
        mDoMtx_stack_c::YrotS(-0x8000);
        mDoMtx_stack_c::revConcat(getLeftItemMatrix());
    }

    model->setBaseTRMtx(mDoMtx_stack_c::get());
    modelCalc(model);
    mpDummyLoadedAmmoModel = model;
}

void daDummyPlayer_c::updateRemoteHeldItemMatrix(const LinkPuppetState& state) {
    if (mHeldItemModel) {
        if (mEquipItem == 0x106) {
            mHeldItemModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4));
        } else if (checkOilBottleItemNotGet(mEquipItem)) {
            mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mRightItemJntNo));
            mDoMtx_stack_c::transM(1.5f, -7.5f, -1.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(183.0f), cM_deg2s(176.0f),
                                    cM_deg2s(167.0f));
            mHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
        } else if (checkBottleItem(mEquipItem)) {
            mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));
            mDoMtx_stack_c::transM(-10.0f, -0.5f, -5.5f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(174.0f), cM_deg2s(-47.0f),
                                    cM_deg2s(94.0f));
            mHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());

            if (mpHookTipModel) {
                simpleAnmPlay(mHookTipBck.getBckAnm());
                mpHookTipModel->setBaseTRMtx(mDoMtx_stack_c::get());
                mpHookTipModel->calc();
            }
            if (state.sendsItemFx) {
                applyRemoteBottle(state.itemFx.hx);
            }
            // The fairy inside glows (its setBottleEffect branch is the only one without a proc).
            if (mEquipItem == dItemNo_FAIRY_e && !isHidden()) {
                setBottleEffect();
            }
        } else if (checkBowAndSlingItem(mEquipItem)) {
            if (checkBowGrabLeftHand()) {
                mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));
                mDoMtx_stack_c::transM(-1.3f, 0.0f, -3.0f);
                mDoMtx_stack_c::XYZrotM(cM_deg2s(-74.0f), cM_deg2s(43.6f),
                                        cM_deg2s(1.9f));
                mHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
            } else {
                mHeldItemModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mRightItemJntNo));
            }
        } else if (checkHookshotItem(mEquipItem)) {
            updateRemoteHookshot(state.itemFx.hk);
        } else if (mEquipItem == dItemNo_IRONBALL_e) {
            updateRemoteIronBall(state.itemFx.bc);
        } else if (mEquipItem == dItemNo_BOOMERANG_e) {
            // daBoomerang_c::setKeepMatrix.
            mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));
            mDoMtx_stack_c::transM(32.0f, -5.0f, -6.0f);
            mDoMtx_stack_c::XYZrotM(cM_deg2s(-4.0f), cM_deg2s(39.0f), cM_deg2s(-9.0f));
            mHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
        } else {
            if (mEquipItem == dItemNo_COPY_ROD_e && field_0x0724) {
                // The head glows while the rod holds a statue's power (setItemMatrix).
                const bool lit = (state.visFlags & twili::kVisCopyRodLit) != 0;
                field_0x0724->setFrame(lit ? field_0x0724->getFrameMax() - 0.001f : 0.0f);
            }

            mHeldItemModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mLeftItemJntNo));
        }

        if (mItemBck.getBckAnm()) {
            if (field_0x33dc >= mItemBck.getBckAnm()->getFrameMax()) {
                field_0x33dc = mItemBck.getBckAnm()->getFrameMax() - 0.001f;
            }
            mItemBck.entry(mHeldItemModel->getModelData(), field_0x33dc);
        }

        mHeldItemModel->calc();
    }

    // daAlink_c::draw's condition for the chains, for the autotest.
    const bool hook = checkHookshotItem(mEquipItem) && mHeldItemModel != nullptr;
    mDummyHookTipDist = hook ? mHeldItemRootPos.abs(mHookshotTopPos) : 0.0f;
    mDummyHookChain = hook && (mDummyHookTipDist > 1.0f || field_0x3810.abs(mIronBallBgChkPos) > 1.0f);
    if (mDummyHookChain && !mDummyHookWasChain) {
        mDummyHookShots++;
        mDummyHookPeak = 0.0f;
    }
    if (mDummyHookChain) {
        mDummyHookPeak = (std::max)(mDummyHookPeak, mDummyHookTipDist);
    }
    mDummyHookWasChain = mDummyHookChain;

    updateRemoteLantern(state);
    updateRemoteLoadedAmmo(state);
}

// setHookshotPos without the flight, the line checks, the sounds and the rumble
void daDummyPlayer_c::updateRemoteHookshot(const twili::RemoteHookshot& hk) {
    using namespace twili;
    static const Vec kHookRoot = {0.0f, 0.0f, 23.5f};
    if (hk.mode != mItemMode || hk.sub != mDummyHookSub) {
        TwiliLog.debug("[dummy {}] clawshot mode {} -> {}, other tip {} -> {}",
                      mDummyClientId, mItemMode, hk.mode, mDummyHookSub, hk.sub);
        mDummyHookSub = hk.sub;
    }
    field_0x3020 = hk.hand;
    mItemMode = hk.mode;
    field_0x3026 = hk.stopTime;  // the chain's wobble after a catch (getHookshotStopTime)

    J3DModel* left = hk.hand == 0 ? mHeldItemModel : field_0x0710;
    J3DModel* right = hk.hand == 0 ? field_0x0710 : mHeldItemModel;
    mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));
    mDoMtx_stack_c::transM(-2.0f, 1.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(5.7f), cM_deg2s(162.0f), 0);
    if (left != nullptr) {
        left->setBaseTRMtx(mDoMtx_stack_c::get());
    }
    mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mRightItemJntNo));
    mDoMtx_stack_c::transM(-2.0f, 0.0f, 1.0f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(-78.0f), cM_deg2s(182.0f), cM_deg2s(-99.0f));
    if (right != nullptr) {
        right->setBaseTRMtx(mDoMtx_stack_c::get());
    }
    if (field_0x0710 != nullptr) {
        if (mItemBck.getBckAnm() != nullptr) {
            mItemBck.entry(field_0x0710->getModelData(), 0.0f);
        }
        field_0x0710->calc();
    }
    mDoMtx_multVec(mHeldItemModel->getBaseTRMtx(), &kHookRoot, &mHeldItemRootPos);
    if (field_0x0710 != nullptr) {
        mDoMtx_multVec(field_0x0710->getBaseTRMtx(), &kHookRoot, &field_0x3810);
    } else {
        field_0x3810 = mHeldItemRootPos;
    }

    if (hk.out()) {
        mHookshotTopPos.set(hk.tip[0], hk.tip[1], hk.tip[2]);
        mDoMtx_stack_c::transS(mHookshotTopPos);
        mDoMtx_stack_c::ZXYrotM(hk.tipAng[0], hk.tipAng[1], 0);
    } else {
        mDoMtx_stack_c::copy(mHeldItemModel->getBaseTRMtx());
        mDoMtx_stack_c::transM(kHookRoot.x, kHookRoot.y, kHookRoot.z);
        mDoMtx_stack_c::multVecZero(&mHookshotTopPos);
    }
    if (mpHookTipModel != nullptr) {
        if (mHookTipBck.getBckAnm() != nullptr) {
            mHookTipBck.entry(mpHookTipModel->getModelData(), hk.tipFrame);
        }
        mpHookTipModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpHookTipModel->calc();
    }

    // The other tip
    f32 subFrame = 0.0f;
    bool subHome = true;
    const cXyz subTip(hk.subTip[0], hk.subTip[1], hk.subTip[2]);
    if (hk.sub == kItemFxHookSubRoof || hk.sub == kItemFxHookSubWall) {
        mIronBallBgChkPos = subTip;
        mDoMtx_stack_c::transS(subTip);
        mDoMtx_stack_c::ZXYrotM(hk.sub == kItemFxHookSubRoof ? -0x4000 : 0, hk.subAng, 0);
        subFrame = 14.0f;
        subHome = false;
    } else if (hk.sub == kItemFxHookSubReturn) {
        const cXyz d = subTip - field_0x3810;
        if (d.abs2() >= 1.0f) {
            mIronBallBgChkPos = subTip;
            mDoMtx_stack_c::transS(subTip);
            mDoMtx_stack_c::ZXYrotM(d.atan2sY_XZ(), d.atan2sX_Z(), 0);
            subHome = false;
        }
    }
    if (subHome) {
        if (field_0x0710 != nullptr) {
            mDoMtx_stack_c::copy(field_0x0710->getBaseTRMtx());
            mDoMtx_stack_c::transM(kHookRoot.x, kHookRoot.y, kHookRoot.z);
        }
        mIronBallBgChkPos = field_0x3810;
    }
    if (field_0x0714 != nullptr) {
        if (mHookTipBck.getBckAnm() != nullptr) {
            mHookTipBck.entry(field_0x0714->getModelData(), subFrame);
        }
        field_0x0714->setBaseTRMtx(mDoMtx_stack_c::get());
        field_0x0714->calc();
    }
}

// setIronBallPos without its physics, collision, sounds and rumble
void daDummyPlayer_c::updateRemoteIronBall(const twili::RemoteIronBall& bc) {
    if (mIronBallChainPos == nullptr || mIronBallChainAngle == nullptr || field_0x3848 == nullptr) {
        return;
    }
    static const Vec kChainVec = {0.0f, 0.0f, 10.0f};   // l_ironBallChainVec
    static const Vec kCenterVec = {0.0f, 0.0f, 42.0f};  // l_ironBallCenterVec
    // The chain's end in the right hand.
    mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(15));
    mDoMtx_stack_c::transM(-1.0f, -6.0f, -3.6f);
    mDoMtx_stack_c::XYZrotM(cM_deg2s(150.0f), cM_deg2s(-81.0f), cM_deg2s(111.0f));
    if (bc.mode == 0) {
        mDoMtx_stack_c::transM(0.0f, 0.0f, 10.0f);
        mDoMtx_stack_c::XrotM(0x7FFF);
    }
    mDoMtx_stack_c::multVec(&kChainVec, &mHookshotTopPos);
    mDoMtx_stack_c::multVecZero(&mHeldItemRootPos);
    mDoMtx_MtxToRot(mDoMtx_stack_c::get(), &field_0x316c);

    if (bc.mode == 0) {
        // Hanging from the left hand.
        mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(10));
        mDoMtx_stack_c::transM(-35.3f, -9.5f, -16.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(61.5f), cM_deg2s(-2.5f), cM_deg2s(50.3f));
        mDoMtx_stack_c::multVecZero(mIronBallChainPos);
        if (bc.aim) {
            mDoMtx_MtxToRot(mDoMtx_stack_c::get(), mIronBallChainAngle);
        } else {
            mIronBallChainAngle->set(-0x4000, shape_angle.y, 0);
        }
        mItemMode = 6;
    } else {
        mIronBallChainPos->set(bc.ball[0], bc.ball[1], bc.ball[2]);
        mIronBallChainAngle->set(bc.ballAng[0], bc.ballAng[1], bc.ballAng[2]);
        mItemMode = bc.links;
    }
    mItemVar0.field_0x3018 = bc.mode == 7 ? 8 : bc.mode;
    mSearchBallScale = 0.0f;
    setIronBallChainPos();
    mItemVar0.field_0x3018 = bc.mode;

    mDoMtx_stack_c::transS(*mIronBallChainPos);
    mDoMtx_stack_c::ZXYrotM(*mIronBallChainAngle);
    mHeldItemModel->setBaseTRMtx(mDoMtx_stack_c::get());
    mDoMtx_stack_c::multVec(&kCenterVec, &mIronBallCenterPos);
    field_0x3810 = mIronBallBgChkPos;
    mIronBallBgChkPos.set(mIronBallCenterPos.x, mIronBallCenterPos.y - 32.0f, mIronBallCenterPos.z);
    if (bc.mode != mDummyIronBallMode) {
        TwiliLog.debug("[dummy {}] ball and chain mode {} -> {} ({} links)", mDummyClientId,
                      mDummyIronBallMode, bc.mode, bc.links);
    }
    mDummyIronBallMode = bc.mode;
    mDummyIronBallDist = mIronBallCenterPos.abs(mHeldItemRootPos);
}

// The bottle's liquid and contents as the remote's show them ("hx")
void daDummyPlayer_c::applyRemoteBottle(const twili::RemoteHeldExtra& hx) {
    J3DAnmTextureSRTKey* const btks[3] = {field_0x0718, field_0x071c, field_0x0720};
    if (hx.bottleBtk != 0 && btks[hx.bottleBtk - 1] != nullptr) {
        J3DAnmTextureSRTKey* btk = btks[hx.bottleBtk - 1];
        if (mDummyBottleBtk != hx.bottleBtk) {
            mHeldItemModel->getModelData()->entryTexMtxAnimator(btk);
            mDummyBottleBtk = hx.bottleBtk;
        }
        btk->setFrame(std::clamp(hx.bottleBtkFrame, 0.0f, static_cast<f32>(btk->getFrameMax())));
    }
    if (field_0x072c != nullptr) {
        field_0x072c->setFrame(
            std::clamp(hx.bottleBtpFrame, 0.0f, static_cast<f32>(field_0x072c->getFrameMax())));
    }
}

// The lit lantern, in hand or hanging on the belt (setItemMatrix).
void daDummyPlayer_c::updateRemoteLantern(const LinkPuppetState& state) {
    const bool inHand = mEquipItem == dItemNo_KANTERA_e;
    const bool onBelt = !inHand && (state.visFlags & twili::kVisLanternBelt) != 0;
    if ((!inHand && !onBelt) || !mpKanteraModel || !mpKanteraGlowModel) {
        if (checkNoResetFlg2(FLG2_UNK_1)) {
            offKandelaarModel();
        }
        stopDrawParticle(field_0x31c4);
        mDummyLanternFlame = false;
        return;
    }

    simpleAnmPlay(mpKanteraGlowBtk);
    if (!checkNoResetFlg2(FLG2_UNK_1)) {
        mDoMtx_multVecZero(mpLinkModel->getAnmMtx(mLeftItemJntNo), &mKandelaarFlamePos);
        mKandelaarFlamePos.y -= 30.0f;
        field_0x3630 = field_0x3624;
        field_0x3624 = mKandelaarFlamePos;
        field_0x3618 = cXyz::Zero;
        field_0x32c8 = 0;
        field_0x3448 = 0.0f;
        field_0x344c = -1.0f;
        mZ2Link.setKanteraState(1);
    }
    onNoResetFlg2(FLG2_UNK_1);
    // An older build sends no flame
    const bool lit = !state.sendsItemFx || (state.visFlags & twili::kVisLanternLit) != 0;
    updateRemoteLanternFlame(state, lit);
    if (inHand) {
        mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(mLeftItemJntNo));
        mDoMtx_stack_c::transM(-2.0f, -0.1f, -0.7f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(100.0f), cM_deg2s(9.3f), cM_deg2s(183.0f));
    } else {
        mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(0x10));
        mDoMtx_stack_c::transM(-1.0f, 4.5f, 9.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(-75.0f), cM_deg2s(62.0f), cM_deg2s(89.0f));
    }
    mpKanteraModel->setBaseTRMtx(mDoMtx_stack_c::get());
    // Its joint callback swings the body and moves mKandelaarFlamePos along.
    modelCalc(mpKanteraModel);

    mDoMtx_stack_c::transS(mKandelaarFlamePos);
    const Vec glowScale = {field_0x3448, field_0x3448, field_0x3448};
    mpKanteraGlowModel->setBaseScale(glowScale);
    mpKanteraGlowModel->setBaseTRMtx(mDoMtx_stack_c::get());
    modelCalc(mpKanteraGlowModel);
}

void daDummyPlayer_c::updateRemoteLanternFlame(const LinkPuppetState& state, bool lit) {
    const bool shown = !isHidden() && !(mDummyVisFlags & twili::kVisNoDraw);
    const bool audible = shown && isRemotePlayerAudioAudible(this);
    if (lit && shown) {
        onNoResetFlg1(FLG1_UNK_80);
        u16 effName = ID_ZI_J_KANTERA_FIRE;
        dPa_levelEcallBack* callback = nullptr;
        if (state.visFlags & twili::kVisLanternSwing) {
            effName = ID_ZI_J_KANTERA_SWINGFIRE;
            // playerInit's, which the dummy never runs
            field_0x2f20.setOldPosP(&field_0x3624, &field_0x3630);
            callback = &field_0x2f20;
            JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(field_0x31c4);
            if (emitter != nullptr && emitter->getEmitterCallBackPtr() == nullptr) {
                emitter->stopDrawParticle();
            }
        }
        field_0x31c4 = dComIfGp_particle_set(field_0x31c4, effName, &mKandelaarFlamePos, &tevStr,
                                             &shape_angle, NULL, 0xFF, callback, -1, NULL, NULL,
                                             NULL);
        if (audible) {
            mZ2Link.getKantera().startLevelSound(Z2SE_AL_KANTERA_BURNING, 0,
                                                 mVoiceReverbIntensity);
        }
    } else {
        if (!lit && checkNoResetFlg1(FLG1_UNK_80) && audible) {
            mZ2Link.getKantera().startSound(Z2SE_AL_KANTERA_OFF, 0, mVoiceReverbIntensity);
        }
        if (!lit) {
            offNoResetFlg1(FLG1_UNK_80);
        }
        stopDrawParticle(field_0x31c4);
    }
    cLib_addCalc(&field_0x3448, lit ? 1.0f : 0.0f, 0.5f, 0.3f, 0.1f);
    mDummyLanternFlame = lit && shown && dComIfGp_particle_getEmitter(field_0x31c4) != nullptr;
}

void daDummyPlayer_c::updateRemoteItemMatrices(const LinkPuppetState& state) {
    if (!mpLinkModel || !mSwordModel || !mSheathModel || !mShieldModel ||
        !mpLinkFaceModel || !mpLinkHatModel)
    {
        return;
    }

    mSheathModel->setBaseTRMtx(mpLinkModel->getAnmMtx(field_0x30b6));
    modelCalc(mSheathModel);

    if (mEquipItem == kSwordEquipItem) {
        mSwordModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mLeftItemJntNo));
    } else {
        mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(field_0x30b6));
        mDoMtx_stack_c::transM(-18.5f, 0.14f, 12.2f);
        mDoMtx_stack_c::XYZrotM(0, cM_deg2s(33.1f), 0);
        mSwordModel->setBaseTRMtx(mDoMtx_stack_c::get());
    }
    modelCalc(mSwordModel);

    const bool shieldInHand = state.shieldInHand || mEquipItem == kSwordEquipItem;
    if (shieldInHand) {
        mShieldModel->setBaseTRMtx(mpLinkModel->getAnmMtx(mRightItemJntNo));
        field_0x2e44.offPassNum(0xF);
        field_0x2e44.onPassNum(0x10);
    } else {
        mDoMtx_stack_c::copy(mpLinkModel->getAnmMtx(field_0x30b6));
        mDoMtx_stack_c::transM(4.2f, -4.4f, -20.0f);
        mDoMtx_stack_c::XYZrotM(cM_deg2s(91.0f), cM_deg2s(57.0f), cM_deg2s(180.0f));
        mShieldModel->setBaseTRMtx(mDoMtx_stack_c::get());
        field_0x2e44.onPassNum(0xF);
        field_0x2e44.offPassNum(0x10);
    }
    modelCalc(mShieldModel);

    updateRemoteHeldItemMatrix(state);

    updateRemoteFace();
    mpLinkFaceModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4));
    modelCalc(mpLinkFaceModel);
    mpLinkHatModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4));
    // setHatAngle reads the hat joints from the last calc.
    const bool primeHat = mDummyHatPrimed != mpLinkHatModel;
    if (primeHat) {
        modelCalc(mpLinkHatModel);
        // The joint setHatAngle follows (sp38 there).
        const int hatJoint =
            checkNoResetFlg2(daPy_FLG2(FLG2_UNK_100000 | FLG2_UNK_80000)) ? 0 : 7;
        mDoMtx_multVecZero(mpLinkHatModel->getAnmMtx(hatJoint), &field_0x34c8);
        onEndResetFlg0(ERFLG0_UNK_800000);
        mDummyHatPrimed = mpLinkHatModel;
    }
    setHatAngle();
    if (primeHat) {
        mEndResetFlg0 &= ~ERFLG0_UNK_800000;
    }
    {
        // The hat squashes while the remote transforms (headModelCallBack).
        const bool hatScale =
            twili::planTransformFx(state.tf, checkWolf() != 0, state.modelSwap).hatScale;
        ScopedRemoteMetamorphose metamorphose(*this, hatScale, mProcVar3.field_0x300e,
                                              state.tf.hatScale);
        modelCalc(mpLinkHatModel);
    }
    updateRemoteBootMatrices();

    setSwordPos();
}

// The idle face of playFaceTextureAnime
void daDummyPlayer_c::updateRemoteFace() {
    if (mpFaceBtp && mpFaceBtk) {
        const int blinkEnd = (std::max)(mpFaceBtp->getFrameMax(), mpFaceBtk->getFrameMax());
        if (mDummyBlinkFrame != 0) {
            if (++mDummyBlinkFrame > blinkEnd) {
                mDummyBlinkFrame = 0;
            }
        } else if (cM_rnd() < kDummyBlinkChance) {
            mDummyBlinkFrame = 1;
        }
        setClampedAnmFrame(mpFaceBtp, static_cast<float>(mDummyBlinkFrame));
        setClampedAnmFrame(mpFaceBtk, static_cast<float>(mDummyBlinkFrame));
    }

    // Not under the lowered Zora helmet, as playFaceTextureAnime.
    J3DAnmTransform* faceBck = mFaceBck.getBckAnm();
    const bool zoraMask = mDummyClothesItem == dItemNo_WEAR_ZORA_e &&
                          (mDummyVisFlags & twili::kVisZoraMask) != 0;
    if (mDummyFaceBckReady && faceBck && !zoraMask) {
        mFaceBck.entry(mpLinkFaceModel->getModelData(),
                       (std::min)(mUnderFrameCtrl[0].getFrame(),
                                  static_cast<float>(faceBck->getFrameMax())));
    }
}

// Each boot follows one leg's lower joints (setItemMatrix).
void daDummyPlayer_c::updateRemoteBootMatrices() {
    if (!checkEquipHeavyBoots()) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        mpLinkBootModels[i]->setBaseTRMtx(mpLinkModel->getBaseTRMtx());
        modelCalc(mpLinkBootModels[i]);
    }

    mpLinkBootModels[0]->setAnmMtx(1, mpLinkModel->getAnmMtx(0x13));
    mpLinkBootModels[0]->setAnmMtx(2, mpLinkModel->getAnmMtx(0x14));
    mpLinkBootModels[0]->setAnmMtx(3, mpLinkModel->getAnmMtx(0x15));

    mDoMtx_stack_c::XrotS(-0x8000);
    if (interp::isEnabled()) {
        // setAnmMtx records the matrix for frame interpolation
        Mtx bootMtx;
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x18), mDoMtx_stack_c::get(), bootMtx);
        mpLinkBootModels[1]->setAnmMtx(1, bootMtx);
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x19), mDoMtx_stack_c::get(), bootMtx);
        mpLinkBootModels[1]->setAnmMtx(2, bootMtx);
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x1A), mDoMtx_stack_c::get(), bootMtx);
        mpLinkBootModels[1]->setAnmMtx(3, bootMtx);
    } else {
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x18), mDoMtx_stack_c::get(),
                      mpLinkBootModels[1]->getAnmMtx(1));
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x19), mDoMtx_stack_c::get(),
                      mpLinkBootModels[1]->getAnmMtx(2));
        mDoMtx_concat(mpLinkModel->getAnmMtx(0x1A), mDoMtx_stack_c::get(),
                      mpLinkBootModels[1]->getAnmMtx(3));
    }
}

static void decayDummySwordBlur(daAlink_blur_c& blur, const cXyz& currentPos,
                                const cXyz& oldPos, s16 angleDelta) {
    static constexpr int kDummySwordBlurDecay = 10;

    if (blur.field_0x14 < kDummySwordBlurDecay) {
        blur.field_0x14 = 0;
    } else {
        blur.field_0x14 -= kDummySwordBlurDecay;
        blur.traceBlur(&currentPos, &oldPos, angleDelta);
    }
}

void daDummyPlayer_c::updateRemoteSwordEffects(const LinkPuppetState& state) {
    if (mDummyHidden) {
        // Frame 0 fades the charge glow out instead of leaving it hanging where the dummy was.
        if (m_nSwordBtk) {
            m_nSwordBtk->setFrame(0.0f);
            if (mSwordModel) {
                setSwordChargeEffect();
            }
        }
        if (mDummyCutTurnEffectWasActive) {
            clearCutTurnEffectID();
            mDummyCutTurnEffectWasActive = false;
        }
        m_swordBlur.field_0x14 = 0;
        mDummySwordBlurWasActive = false;
        return;
    }

    if (m_nSwordBtk) {
        if (state.swordChargeActive && mDummySwordItem == dItemNo_SWORD_e &&
            mEquipItem == kSwordEquipItem)
        {
            setClampedAnmFrame(m_nSwordBtk, state.swordChargeFrame);
        } else {
            m_nSwordBtk->setFrame(0.0f);
        }
    }

    if (isMasterSwordItem(mDummySwordItem)) {
        advanceSimpleAnm(m_mSwordBtk);
        advanceSimpleAnm(m_mSwordBrk);
    } else {
        if (m_mSwordBtk) {
            m_mSwordBtk->setFrame(0.0f);
        }
        if (m_mSwordBrk) {
            m_mSwordBrk->setFrame(0.0f);
        }
    }

    if (m_nSwordBtk && mSwordModel) {
        setSwordChargeEffect();
    }

    const bool swordAttackActive =
        state.swordBlurActive && mSwordModel && mEquipItem == kSwordEquipItem &&
        m_swordBlur.m_blurTex;
    const int fallbackBlurAlpha =
        checkCutDashAnime() ? mpHIO->mCut.m.mDashBlurAlpha : mpHIO->mCut.m.mBlurAlpha;
    m_swordBlur.field_0x20 =
        swordAttackActive && state.swordBlurAlpha == 0 ? fallbackBlurAlpha : state.swordBlurAlpha;

    if (!swordAttackActive) {
        decayDummySwordBlur(m_swordBlur, current.pos, old.pos,
                            static_cast<s16>(shape_angle.y - old.angle.y));
        mDummySwordBlurWasActive = false;
    } else {
        static constexpr int kDummySwordBlurMaxSegments = 50;
        static constexpr float kDummySwordBlurResetDistance = 420.0f;

        const bool resetTrail =
            !mDummySwordBlurWasActive ||
            m_swordBlur.field_0x14 <= 0 ||
            m_swordBlur.field_0x38[0].abs(mSwordTopPos) > kDummySwordBlurResetDistance;
        if (resetTrail) {
            m_swordBlur.initBlur(0.0f, 0, &mSwordTopPos, &field_0x3498, &field_0x34a4);
        } else {
            m_swordBlur.copyBlur(&mSwordTopPos, &field_0x3498, &field_0x34a4);
            if (m_swordBlur.field_0x14 > kDummySwordBlurMaxSegments) {
                m_swordBlur.field_0x14 = kDummySwordBlurMaxSegments;
            }
        }
        mDummySwordBlurWasActive = true;
    }

    const bool rightSpin =
        state.cutType == daPy_py_c::CUT_TYPE_TURN_RIGHT ||
        state.cutType == daPy_py_c::CUT_TYPE_LARGE_TURN_RIGHT;
    const bool leftSpin =
        state.cutType == daPy_py_c::CUT_TYPE_TURN_LEFT ||
        state.cutType == daPy_py_c::CUT_TYPE_LARGE_TURN_LEFT;
    const bool spinActive = swordAttackActive && (rightSpin || leftSpin);
    if (spinActive) {
        mProcVar1.field_0x300a = rightSpin ? 6 : 8;
        mProcVar4.field_0x3010 = rightSpin ? 1 : 0;
        setCutTurnEffect();
        mDummyCutTurnEffectWasActive = true;
    } else if (mDummyCutTurnEffectWasActive) {
        clearCutTurnEffectID();
        mDummyCutTurnEffectWasActive = false;
    }
}

void daDummyPlayer_c::updateRemoteAudio(const LinkPuppetState& state) {
    if (!isRemotePlayerAudioAudible(this) || mDummyHidden) {
        resetBasAnime();
        ScopedDummyLinkAudioPtr scoped(&mZ2Link);
        mZ2Link.initAnime(nullptr, true, 0.0f, 0.0f);
        return;
    }

    const bool swordAudioActive =
        state.swordBlurActive || state.cutType != daPy_py_c::CUT_TYPE_NONE;
    const bool useUpperBas =
        swordAudioActive && mDummyUpperDominantPack >= 0 &&
        mDummyUpperDominantPack < kAnmPackCount &&
        isValidAnm(mDummyUpperActiveAnm[mDummyUpperDominantPack]);
    const int basPack = useUpperBas ? mDummyUpperDominantPack : mDummyLowerDominantPack;

    int roomNo = dStage_roomControl_c::getStayNo();
    if (roomNo < 0) {
        roomNo = 0;
    }

    mPolySound = 0;
    mVoiceReverbIntensity = dComIfGp_getReverb(roomNo);

    if (basPack < 0 || basPack >= kAnmPackCount)
    {
        resetBasAnime();
        return;
    }

    const bool basReady = setRemoteBasAnime(useUpperBas, basPack);

    {
        ScopedDummyLinkAudioPtr scoped(&mZ2Link);
        mZ2Link.framework(mPolySound, mVoiceReverbIntensity);
        if (basReady && field_0x2d7c != nullptr) {
            mZ2Link.updateAnime(field_0x2d7c->getFrame(), field_0x2d7c->getRate());
        }
    }
}

void daDummyPlayer_c::playRemotePlayerSfx(uint32_t soundId, uint8_t kind, uint32_t mapInfo) {
    if (!isRemotePlayerAudioAudible(this) || isHidden()) {
        return;
    }

    int roomNo = dStage_roomControl_c::getStayNo();
    if (roomNo < 0) {
        roomNo = 0;
    }

    mVoiceReverbIntensity = dComIfGp_getReverb(roomNo);
    ScopedDummyLinkAudioPtr scoped(&mZ2Link);
    switch (static_cast<twili::PlayerSfxKind>(kind)) {
    case twili::PlayerSfxKind::Voice:
        mZ2Link.startLinkVoice(soundId, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::VoiceLevel:
        mZ2Link.startLinkVoiceLevel(soundId, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::Sword:
        mZ2Link.startLinkSwordSound(soundId, mapInfo, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::Sound:
        mZ2Link.startLinkSound(soundId, 0, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::SoundLevel:
        mZ2Link.startLinkSoundLevel(soundId, 0, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::MapInfo:
        mZ2Link.startLinkSound(soundId, mapInfo, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::MapInfoLevel:
        mZ2Link.startLinkSoundLevel(soundId, mapInfo, mVoiceReverbIntensity);
        break;
    case twili::PlayerSfxKind::MidnaVoice:
    case twili::PlayerSfxKind::MidnaSound:
        break;
    }
}

void daDummyPlayer_c::playRemoteSfx(uint32_t soundId, uint8_t kind, uint32_t mapInfo) {
    if (kind == static_cast<uint8_t>(twili::PlayerSfxKind::MidnaVoice) ||
        kind == static_cast<uint8_t>(twili::PlayerSfxKind::MidnaSound))
    {
        if (isRemotePlayerAudioAudible(this) && !isHidden()) {
            mDummyMidna.playSfx(soundId, kind == static_cast<uint8_t>(twili::PlayerSfxKind::MidnaVoice),
                mapInfo, current.pos);
        }
        return;
    }
    playRemotePlayerSfx(soundId, kind, mapInfo);
}

// Each queued PLAYER_SFX once the pose shown reaches the tick it was made in.
void daDummyPlayer_c::playQueuedSfx(const twili::Client& client, double shownSeq, bool snapped) {
    static constexpr int32_t kMaxLateTicks = 6;
    static constexpr int32_t kMaxAheadTicks = 90;
    const twili::RemoteSfxQueue& queue = client.sfx;
    if (!mDummySfxPrimed) {
        mDummySfxDone = queue.lastIndex();
        mDummySfxPrimed = true;
        return;
    }
    const uint32_t shownTick = static_cast<uint32_t>((std::max)(0.0, std::floor(shownSeq)));
    for (size_t i = 0; i < queue.size(); i++) {
        const twili::RemoteSfx& s = queue.at(i);
        if (static_cast<int32_t>(s.index - mDummySfxDone) <= 0) {
            continue;
        }
        const int32_t ahead = static_cast<int32_t>(s.seq - shownTick);
        if (ahead > 0 && ahead <= kMaxAheadTicks) {
            break;
        }
        mDummySfxDone = s.index;
        if (ahead > 0 || ahead < -kMaxLateTicks || snapped) {
            mDummySfxDropped++;
            continue;
        }
        playRemoteSfx(s.id, s.kind, s.mapInfo);
        mDummySfxPlayed++;
        std::copy_backward(std::begin(mDummySfxRecent), std::end(mDummySfxRecent) - 1,
            std::end(mDummySfxRecent));
        mDummySfxRecent[0] = {s.id, s.seq, shownSeq, s.kind};
    }
}

// PvP: our hit landing, from the dummy's own sound object (def_se_set, or setGuardSe's clang).
void daDummyPlayer_c::playPvpHitSe(uint32_t hitSe, uint32_t guardSe, bool blocked) {
    if (!isRemotePlayerAudioAudible(this) || isHidden()) {
        return;
    }
    const bool wood = mDummyShieldItem == dItemNo_WOOD_SHIELD_e;
    const u32 soundId = blocked && !wood ? guardSe : hitSe;
    const u32 mapInfo = !blocked ? 30 : wood ? 0x29 : 0x28;
    if (soundId == 0) {
        return;
    }
    ScopedDummyLinkAudioPtr scoped(&mZ2Link);
    // Z2Creature's, not Z2CreatureLink's: that one also drives our battle music.
    mZ2Link.Z2Creature::startCollisionSE(soundId, mapInfo);
}

// Each set's textures come from this dummy's private archives, so they are its own to rewrite.
void daDummyPlayer_c::bindRemoteRecolor() {
    using twili::RecolorSetId;
    JKRHeap* heap = mDoExt_getCurrentHeap();
    auto bind = [&](RecolorSetId id, J3DModel* body, J3DModel* second) {
        J3DModelData* const models[2] = {body ? body->getModelData() : nullptr,
                                         second ? second->getModelData() : nullptr};
        if (models[0] != nullptr) {
            mDummyRecolor[static_cast<size_t>(id)].bind(id, models, heap, mDummyClientId);
        }
    };
    bind(RecolorSetId::Kokiri, mpDummyKokiriLinkModel, mpDummyKokiriHatModel);
    bind(RecolorSetId::Zora, mpDummyZoraLinkModel, mpDummyZoraHatModel);
    bind(RecolorSetId::Magic, mpDummyMagicLinkModel, mpDummyMagicHatModel);
    bind(RecolorSetId::Ordon, mpDummyCasualLinkModel, nullptr);
    // The four chains share one model data.
    bind(RecolorSetId::Wolf, mpDummyWolfModel, mpDummyWolfChainModels[0]);
}

twili::RecolorSetId daDummyPlayer_c::wornRecolorSet() const {
    using twili::RecolorSetId;
    if (checkWolf()) {
        return RecolorSetId::Wolf;
    }
    switch (mDummyClothesItem) {
    case dItemNo_WEAR_ZORA_e: return RecolorSetId::Zora;
    case dItemNo_ARMOR_e: return RecolorSetId::Magic;
    case dItemNo_WEAR_CASUAL_e: return RecolorSetId::Ordon;
    default: return RecolorSetId::Kokiri;
    }
}

// A colour dragged in the picker arrives at up to 4 Hz
static constexpr int32_t kRecolorMinTicks = 3;

void daDummyPlayer_c::updateRemoteRecolor() {
    ++mDummyRecolorTick;
    const twili::RecolorSetId id = wornRecolorSet();
    const bool justWorn = mDummyRecolorWorn != static_cast<int8_t>(id);
    mDummyRecolorWorn = static_cast<int8_t>(id);
    twili::RecolorSet& set = mDummyRecolor[static_cast<size_t>(id)];
    const uint32_t key = twili::recolorKey(mDummyColorR, mDummyColorG, mDummyColorB);
    if (!set.bound() || set.appliedKey() == key) {
        return;
    }
    // A set just put on is never held back
    if (!justWorn && mDummyRecolorTick - set.lastApplyTick() < kRecolorMinTicks) {
        return;
    }
    set.apply(key, mDummyRecolorTick);
}

void daDummyPlayer_c::getRecolorProbe(twili::RecolorProbe& out) const {
    using twili::RecolorSetId;
    out = {};
    uint32_t bytes = 0;
    for (const twili::RecolorSet& set : mDummyRecolor) {
        bytes += set.bytes();
    }
    const RecolorSetId id = wornRecolorSet();
    mDummyRecolor[static_cast<size_t>(id)].probe(out);
    out.set = static_cast<int8_t>(id);
    out.bytes = bytes;
    out.draws = mDummyDrawCount;
    // The materials vanilla setWaterDropColor writes C1 of (its wet-cloth darkening).
    static constexpr u16 kKokiriBody[] = {17, 9, 0, 1, 2, 16, 15, 14};
    static constexpr u16 kZoraBody[] = {13, 0, 1};
    static constexpr u16 kMagicBody[] = {11, 10, 9, 8, 6};
    static constexpr u16 kOrdonBody[] = {7, 5};
    static constexpr u16 kHat0[] = {0};
    static constexpr u16 kHat1[] = {1};
    static constexpr u16 kMagicHat[] = {2, 1};
    struct List {
        const u16* body;
        size_t bodyCount;
        const u16* hat;
        size_t hatCount;
    };
    List list{};
    switch (id) {
    case RecolorSetId::Kokiri: list = {kKokiriBody, std::size(kKokiriBody), kHat0, 1}; break;
    case RecolorSetId::Zora: list = {kZoraBody, std::size(kZoraBody), kHat1, 1}; break;
    case RecolorSetId::Magic:
        list = {kMagicBody, std::size(kMagicBody), kMagicHat, std::size(kMagicHat)};
        break;
    case RecolorSetId::Ordon: list = {kOrdonBody, std::size(kOrdonBody), kHat0, 1}; break;
    default: break;
    }
    auto scan = [&](J3DModel* model, const u16* mats, size_t count) {
        J3DModelData* data = model ? model->getModelData() : nullptr;
        for (size_t i = 0; data != nullptr && i < count; i++) {
            if (mats[i] >= data->getMaterialNum()) {
                continue;
            }
            const J3DGXColorS10* c = data->getMaterialNodePointer(mats[i])->getTevColor(1);
            for (int v : {c->r, c->g, c->b}) {
                out.c1MaxAbs = std::max(out.c1MaxAbs, static_cast<int16_t>(std::abs(v)));
            }
        }
    };
    scan(mpLinkModel, list.body, list.bodyCount);
    scan(mpLinkHatModel, list.hat, list.hatCount);
}

bool GetDummyPlayerRecolorProbe(fopAc_ac_c* actor, RecolorProbe& out) {
    if (!isDummyPlayer(actor)) {
        return false;
    }
    static_cast<daDummyPlayer_c*>(actor)->getRecolorProbe(out);
    return true;
}

bool IsDummyPlayerHidden(fopAc_ac_c* actor) {
    return isDummyPlayer(actor) &&
           static_cast<daDummyPlayer_c*>(actor)->isHidden();
}

bool IsDummyPlayerShown(fopAc_ac_c* actor) {
    return isDummyPlayer(actor) &&
           static_cast<daDummyPlayer_c*>(actor)->isBodyShown();
}

bool GetDummyPlayerDebugInfo(fopAc_ac_c* actor, DummyPlayerDebugInfo& out) {
    if (!isDummyPlayer(actor)) {
        return false;
    }

    static_cast<daDummyPlayer_c*>(actor)->getDebugInfo(out);
    return true;
}

void PlayDummyPlayerHitSe(fopAc_ac_c* actor, uint32_t hitSe, uint32_t guardSe, bool blocked) {
    if (isDummyPlayer(actor)) {
        static_cast<daDummyPlayer_c*>(actor)->playPvpHitSe(hitSe, guardSe, blocked);
    }
}

void daDummyPlayer_c::getDebugInfo(twili::DummyPlayerDebugInfo& out) const {
    out.shellReady = mDummyShellReady;
    out.remoteWolf = mDummyWasWolf;
    out.wolfBody = checkWolf() != 0;
    out.hidden = isHidden();
    out.bodyJointNum = mpLinkModel ? bodyJointNum() : 0;
    out.refusedAnms = mDummyRefusedAnmCount;
    out.shownSeq = std::max(mDummyPlayout.renderSeq, mDummyPlayout.holdSeq);
    out.missingAnms = mDummyMissingAnmCount;
    // A pack holds a clip exactly while its active id is valid.
    out.basePack = isValidAnm(mDummyLowerActiveAnm[0]);
    out.basePackAnm = mDummyLowerActiveAnm[0];
    out.standIn = mDummyBasePackFallback;
    out.clothes = mDummyClothesItem;
    out.colorR = mDummyColorR;
    out.colorG = mDummyColorG;
    out.colorB = mDummyColorB;
    const twili::RecolorSetId worn = wornRecolorSet();
    out.recolorBound = mDummyRecolor[static_cast<size_t>(worn)].bound();
    out.recolorSet = static_cast<int8_t>(worn);
    out.recolorKey = mDummyRecolor[static_cast<size_t>(worn)].appliedKey();
    out.armorDrained = mDummyArmorWorn && mDummyArmorBrkStatus == 0;
    if (mDummyArmorWorn && mMagicArmorBodyBrk) {
        out.armorBrkFrame = mMagicArmorBodyBrk->getFrame();
        out.armorBrkSettled = out.armorBrkFrame >= mMagicArmorBodyBrk->getFrameMax();
    }
    out.casualHead = checkNoResetFlg2(FLG2_UNK_100000) != 0;
    out.heavyBoots = checkEquipHeavyBoots() != 0;
    out.zoraMask = field_0x06e4 != nullptr && !checkWolf() &&
                   (mDummyVisFlags & twili::kVisZoraMask) != 0;
    out.lantern = checkNoResetFlg2(FLG2_UNK_1) != 0;
    out.heldItem = mDummyHeldItem;
    out.ground = mDummyGroundY > -G_CM3D_F_INF;
    out.midnaReady = mDummyMidna.ready();
    out.midnaMode = mDummyMidna.mode();
    out.midnaShown = isBodyShown() && ((checkWolf() && out.midnaMode == twili::kMidnaDrawn) ||
                                       out.midnaMode == twili::kMidnaApart);
    out.midnaApartTicks = mDummyMidna.apartTicks();
    out.midnaShownTicks = mDummyMidna.shownTicks();
    out.midnaEyeMove = mDummyMidna.eyeMoving();
    out.midnaBodyAnm = mDummyMidna.bodyId();
    out.midnaUpperAnm = mDummyMidna.upperId();
    out.midnaHairHand = mDummyMidna.hairHand();
    out.midnaLeftHand = mDummyMidna.leftHand();
    out.midnaRightHand = mDummyMidna.rightHand();
    out.midnaTired = mDummyMidna.tired();
    out.midnaRefused = mDummyMidna.refused();
    out.midnaBackDist = mDummyMidna.backDist();
    out.hurtbox = mDummyHurtboxLive;
    out.hurtboxGuard = mDummyHurtboxGuard;
    out.tf = mDummyTfTrace;
    out.tfAlive = 0;
    for (int i = 0; i < 2; i++) {
        if (dComIfGp_particle_getEmitter(mDummyTfEmitter[i]) != nullptr) {
            out.tfAlive |= 1 << i;
        }
    }
    out.statusFlags = mDummyStatus.flags;
    out.frozen = checkFreezeDamage() != 0;
    out.iceBlock = dComIfGp_particle_getEmitter(field_0x3268) != nullptr;
    out.thaws = mDummyThaws;
    out.firePoints = 0;
    out.fireEmitters = 0;
    out.fireReceived = 0;
    for (int i = 0; i < 4; i++) {
        const firePointEff_c& e = field_0x32d8[i];
        if (e.field_0x0 != 0) {
            out.firePoints++;
            if (dComIfGp_particle_getEmitter(e.field_0x4) != nullptr) {
                out.fireEmitters++;
            }
        }
        if (mDummyStatus.fire[i].active()) {
            out.fireReceived++;
        }
    }
    out.shieldBurn = field_0x2fcb;
    out.shieldBurnFx = field_0x2fcb != 0 && dComIfGp_particle_getEmitter(field_0x3260[0]) != nullptr;
    out.shieldBurnOuts = mDummyShieldBurnOuts;
    out.shieldItem = mDummyShieldItem;
    out.elec = mDummyElecShown;
    out.elecFx = mDummyElecShown && dComIfGp_particle_getEmitter(field_0x31d8[0]) != nullptr;
    out.damageTimer = static_cast<uint8_t>(std::clamp<int>(mDamageTimer, 0, 255));
    out.flashes = mDummyFlashes;
    out.iceWait = static_cast<uint8_t>(std::clamp<int>(mIceDamageWaitTimer, 0, 255));
    out.sinkOffset = mSinkShapeOffset;
    out.wolfSpin = mDummyWolfSpinWasActive ? mDummyWolfLastSpin : 0;
    out.wolfSpinTicks = mDummyWolfSpinTicks;
    out.wolfLastSpin = mDummyWolfLastSpin;
    out.wolfSpinEmitters = mDummyWolfSpinEmitters;
    out.wolfDome = mDummyHeldItem == twili::wolffx::kLockDomeItem && mHeldItemModel;
    out.wolfDomeShown = out.wolfDome && isBodyShown();
    out.wolfDomeRadius = out.wolfDome ? mSearchBallScale : 0.0f;
    out.wolfLockBlurAlpha = mDummyLockBlurAlpha;
    out.wolfLockDashes = mDummyLockDashes;
    out.midnaHairAim = mDummyMidna.hairAimApplied();
    out.midnaHairAimAngle = mDummyMidna.hairAimAngle();
    out.itemFx = mDummyItemFx.debug();
    out.heapUsed = mDummyHeapUsed;
    const bool hook = checkHookshotItem(mEquipItem) && mHeldItemModel != nullptr && !checkWolf();
    out.hookChain = hook && mDummyHookChain;
    out.hookTipDist = hook ? mDummyHookTipDist : 0.0f;
    out.hookShots = mDummyHookShots;
    out.hookPeak = mDummyHookPeak;
    const bool ironBall = mEquipItem == dItemNo_IRONBALL_e && mHeldItemModel != nullptr;
    out.ironBallMode = ironBall ? mDummyIronBallMode : 0;
    out.ironBallDist = ironBall ? mDummyIronBallDist : 0.0f;
    out.lanternFlame = mDummyLanternFlame && checkNoResetFlg2(FLG2_UNK_1) && !checkWolf();
    out.lanternGlow = checkNoResetFlg2(FLG2_UNK_1) ? field_0x3448 : 0.0f;
    out.sfxPlayed = mDummySfxPlayed;
    out.sfxDropped = mDummySfxDropped;
    out.midnaSfx = mDummyMidna.sfxPlayed();
    out.bowTiltTicks = mDummyBowTiltTicks;
    out.bowTiltDeg = mDummyBowTiltDeg;
    std::copy(std::begin(mDummySfxRecent), std::end(mDummySfxRecent), std::begin(out.sfxRecent));
}

bool daDummyPlayer_c::isHidden() const {
    return mDummyHidden || twili::hideRemotePlayersForCutscene();
}

bool daDummyPlayer_c::isBodyShown() const {
    return mDummyShellReady && !isHidden() && !(mDummyVisFlags & twili::kVisNoDraw);
}

void daDummyPlayer_c::updateRemoteHidden(const twili::Client& client,
                                         const LinkPuppetState& state) {
    // The remote's own draw() skips its body while loadModelDVD swaps it.
    const bool transformSwap = state.modelSwap && state.tf.active() && client.sendsTransformFx;
    mDummyHidden = (state.modelSwap && !transformSwap) ||
                   (state.transformStatus != 0 && !mpDummyWolfModel) ||
                   twili::hideRemotePlayersForCutscene() ||
                   twili::hideRemoteClientForCutscene(client);
}

// After a jump in the remote's pose nothing may blend from the old one
void daDummyPlayer_c::resetRemoteContinuity() {
    mDummyAnimationBlendInitialized = false;
    for (int i = 0; i < kAnmPackCount; i++) {
        mDummyLowerPrevFrameValid[i] = false;
        mDummyUpperPrevFrameValid[i] = false;
    }
    mDummySwordBlurWasActive = false;
    m_swordBlur.field_0x14 = 0;
    mDummyMidna.resetContinuity();
}

// The joints and height changeWolf and changeLink set for each body.
void daDummyPlayer_c::setRemoteBodyMetrics(bool wolf) {
    if (wolf) {
        mHeight = 115.0f;
        mLeftHandJntNo = 19;
        mRightHandJntNo = 24;
        mLeftItemJntNo = 19;
        mRightItemJntNo = 24;
        field_0x30bc = 31;
        field_0x30be = 36;
        field_0x30b6 = 2;
        field_0x32c4[0] = 1;
        field_0x32c4[1] = 2;
        attention_info.field_0xa = 50;
        field_0x3458 = -60.0f;  // a sink deeper than this lifts the attention point
    } else {
        mHeight = 180.0f;
        mLeftHandJntNo = 9;
        mRightHandJntNo = 14;
        mLeftItemJntNo = 10;
        mRightItemJntNo = 15;
        field_0x30bc = 21;
        field_0x30be = 26;
        field_0x30b6 = 5;
        field_0x32c4[0] = 2;
        field_0x32c4[1] = 16;
        attention_info.field_0xa = 10;
        field_0x3458 = -120.0f;
    }
    field_0x30c4 = 3;
    field_0x30b4 = 4;
}

// Binds the body the remote's pose was made for.
void daDummyPlayer_c::applyRemoteForm(bool wolf, uint8_t clothesItem) {
    if (wolf == (checkWolf() != 0) || (wolf && !mpDummyWolfModel)) {
        return;
    }

    // Nothing crosses skeletons
    clearRemoteHeldItemModel();
    offKandelaarModel();
    mpDummyLoadedAmmoModel = nullptr;
    if (mDummyCutTurnEffectWasActive) {
        clearCutTurnEffectID();
        mDummyCutTurnEffectWasActive = false;
    }
    clearRemoteWolfFx();
    forgetRemoteAnimationPacks();
    for (int i = 0; i < kAnmPackCount; i++) {
        mDummyLowerRefusedAnm[i] = kNoAnm;
        mDummyUpperRefusedAnm[i] = kNoAnm;
    }
    field_0x2060->offOldFrameFlg();
    resetRemoteContinuity();
    resetBasAnime();
    mDummyMidna.deactivate();
    // Fire points name joints of the old skeleton (changeCommon puts the remote's out too).
    clearRemoteStatus();

    if (wolf) {
        mpLinkModel = mpDummyWolfModel;
        mpLinkModel->setUserArea(reinterpret_cast<uintptr_t>(this));
        onNoResetFlg1(FLG1_IS_WOLF);
        changeModelDataDirectWolf(0);
        // The eye matAnms move over without the init() changeCommon gives them
        field_0x064C->getMaterialNodePointer(4)->setMaterialAnm(field_0x2180[0]);
        field_0x064C->getMaterialNodePointer(5)->setMaterialAnm(field_0x2180[1]);
        for (int i = 0; i < 4; i++) {
            mpWlChainModels[i] = mpDummyWolfChainModels[i];
        }
        offNoResetFlg1(FLG1_UNK_200000);
        mZ2Link.setLinkState(1);
    } else {
        offNoResetFlg1(FLG1_IS_WOLF);
        // A body other than the one bound, so this binds it and its face, hat and hands.
        applyRemoteClothes(clothesItem);
    }
    setRemoteBodyMetrics(wolf);
}

// The particle of each emitter setMetamorphoseEffect sets (d_a_alink_effect.inc).
static u16 transformFxParticle(twili::TfEmitter e) {
    switch (e) {
    case twili::TfEmitter::AtowA:
        return ID_ZI_J_ATOW_A;
    case twili::TfEmitter::AtowB:
        return ID_ZI_J_ATOW_B;
    case twili::TfEmitter::WtoaA:
        return ID_ZI_J_WTOA_A;
    case twili::TfEmitter::WtoaB:
    default:
        return ID_ZI_J_WTOA_B;
    }
}

// The remote's setMetamorphoseEffect, and the silhouette its draw() shows while its body is swapped
void daDummyPlayer_c::updateRemoteTransformFx(const twili::Client& client,
                                              const LinkPuppetState& state, double shownSeq) {
    using namespace twili;
    const bool wasActive = mDummyTfPlan.active;
    mDummyTfSilhouette = false;
    if (!client.sendsTransformFx) {
        mDummyTf = RemoteTransformFx{};
        mDummyTfPlan = TransformFxPlan{};
        traceRemoteTransformFx(state, 0, shownSeq, wasActive);
        return;
    }
    const bool bodyWolf = checkWolf() != 0;
    mDummyTf = state.tf;
    mDummyTfPlan = planTransformFx(state.tf, bodyWolf, state.modelSwap);
    const TransformFxPlan& plan = mDummyTfPlan;
    if (plan.active && !wasActive) {
        // A new transformation
        mDummyTfAnchorValid = false;
        DummyTransformFxTrace fresh;
        fresh.count = mDummyTfTrace.count + 1;
        mDummyTfTrace = fresh;
    }
    if (!plan.active || isHidden()) {
        // A tick of the old body's clip that went by unseen leaves our frozen point stale
        if (plan.anchor == TfAnchor::LiveJoint2) {
            mDummyTfAnchorValid = false;
        }
        traceRemoteTransformFx(state, 0, shownSeq, wasActive);
        return;
    }

    if (plan.anchor == TfAnchor::LiveJoint2) {
        // Before the swap
        mDoMtx_multVecZero(mpLinkModel->getAnmMtx(2), &mDummyTfAnchor);
        cMtx_copy(mpLinkModel->getAnmMtx(2), mDummyTfJointMtx);
        mDummyTfAnchorValid = true;
    } else if (!mDummyTfAnchorValid) {
        // First seen in or after the swap (a new dummy, or hidden until now)
        if ((state.tf.flags & kTfAnchorValid) != 0) {
            mDummyTfAnchor.set(state.tf.anchor[0], state.tf.anchor[1], state.tf.anchor[2]);
            mDummyTfTrace.anchorSource = 1;
        } else {
            mDummyTfAnchor.set(current.pos.x, current.pos.y + (bodyWolf ? 60.0f : 100.0f),
                               current.pos.z);
            mDummyTfTrace.anchorSource = 2;
        }
        mDoMtx_stack_c::transS(mDummyTfAnchor);
        cMtx_copy(mDoMtx_stack_c::get(), mDummyTfJointMtx);
        mDummyTfAnchorValid = true;
    }

    uint8_t emitted = 0;
    for (int i = 0; i < 2; i++) {
        const TfEmitter e = plan.slot[i];
        if (e == TfEmitter::None) {
            continue;
        }
        if (e == TfEmitter::WtoaB) {
            JPABaseEmitter* emitter =
                setEmitter(&mDummyTfEmitter[i], ID_ZI_J_WTOA_B, &current.pos, NULL);
            if (emitter != NULL) {
                emitter->setGlobalRTMatrix(mDummyTfJointMtx);
            }
        } else {
            setEmitter(&mDummyTfEmitter[i], transformFxParticle(e), &mDummyTfAnchor, NULL);
        }
        emitted |= tfEmitterBit(e);
    }

    if (plan.silhouette && mpWlChangeModel != nullptr) {
        // daAlink_c::execute's matrix for it.
        mDoMtx_stack_c::copy(mpLinkModel->getBaseTRMtx());
        mDoMtx_stack_c::transM(0.0f, 0.0f, plan.silhouetteZ);
        mpWlChangeModel->setBaseTRMtx(mDoMtx_stack_c::get());
        mpWlChangeModel->calc();
        mDummyTfSilhouette = true;
    }
    traceRemoteTransformFx(state, emitted, shownSeq, wasActive);
}

void daDummyPlayer_c::traceRemoteTransformFx(const LinkPuppetState& state, uint8_t emitted,
                                             double shownSeq, bool wasActive) {
    using namespace twili;
    DummyTransformFxTrace& t = mDummyTfTrace;
    const TransformFxPlan& plan = mDummyTfPlan;
    t.flags = mDummyTf.flags;
    t.emitted = emitted;
    t.silhouette = mDummyTfSilhouette;
    t.bodyTev = plan.bodyTev ? mDummyTf.tev : 0;
    t.hatScale = plan.hatScale ? mDummyTf.hatScale : 1.0f;
    const uint16_t baseAnm = mDummyLowerActiveAnm[0];
    const bool clipStarted = baseAnm != mDummyTfClipAnm &&
                             (baseAnm == dRes_ID_ALANM_BCK_CHANGEATOW_e ||
                              baseAnm == dRes_ID_ALANM_BCK_WL_CHANGEWTOA_e);
    mDummyTfClipAnm = baseAnm;
    if (!plan.active) {
        if (wasActive && t.endShown == 0.0) {
            t.endShown = shownSeq;
        }
        return;
    }
    if (clipStarted && t.clipShown == 0.0) {
        t.clipShown = shownSeq;
    }
    if (isHidden()) {
        t.hiddenTicks++;
        return;
    }
    const bool fur = mDummyHeldItem == kMetamorphoseItem;
    if (mDummyTf.postSwap()) {
        if (t.ticksC++ == 0 && t.anchorSource == 0 && (state.tf.flags & kTfAnchorValid) != 0) {
            const cXyz wire(state.tf.anchor[0], state.tf.anchor[1], state.tf.anchor[2]);
            t.anchorErr = (mDummyTfAnchor - wire).abs();
        }
        t.maskC |= emitted;
        if (emitted != 0 && t.postShown == 0.0) {
            t.postShown = shownSeq;
        }
        t.tevMinC = (std::min)(t.tevMinC, t.bodyTev);
        t.tevMaxC = (std::max)(t.tevMaxC, t.bodyTev);
        t.furC |= fur;
    } else if (state.modelSwap) {
        t.silhouetteTicks += mDummyTfSilhouette ? 1 : 0;
        t.maskSwap |= emitted;
        if (emitted != 0 && t.swapShown == 0.0) {
            t.swapShown = shownSeq;
        }
    } else {
        t.ticksA++;
        t.maskA |= emitted;
        if (emitted != 0 && t.startShown == 0.0) {
            t.startShown = shownSeq;
        }
        t.tevMinA = (std::min)(t.tevMinA, t.bodyTev);
        t.furA |= fur;
    }
}

void daDummyPlayer_c::updateRemoteStatus(const LinkPuppetState& state) {
    using namespace twili;
    const RemoteStatusFx& s = state.status;
    // Effects follow the body
    const bool shown = isBodyShown() && !state.modelSwap && dComIfGp_getPlayer(0) != nullptr;
    if (!mDummyStatusPrimed) {
        mDummyShieldBurnOutSeq = s.shieldBurnOutSeq;
        for (int i = 0; i < 4; i++) {
            mDummyFireGen[i] = s.fire[i].gen;
        }
        mDummyStatusPrimed = true;
    }

    if (s.flags & kStatusFrozen) {
        onNoResetFlg1(FLG1_FREEZE_DAMAGE);
    } else {
        offNoResetFlg1(FLG1_FREEZE_DAMAGE);
    }
    // The remote only sends a flash its own draw shows.
    bool absorbed;
    {
        ScopedSelectEquip equip(mDummyClothesItem, mDummySwordItem, mDummyShieldItem);
        absorbed = checkMagicArmorNoDamage() != FALSE;
    }
    if (mDamageTimer == 0 && !absorbed && s.damageTimer != 0) {
        mDummyFlashes++;
    }
    mDamageTimer = absorbed ? 0 : s.damageTimer;
    mDamageColorTime = absorbed ? 0 : s.damageColorTime;
    mIceDamageWaitTimer = s.iceWait;

    updateRemoteFreezeEffect(s, shown);
    // Before the fire points
    updateRemoteShieldBurn(s, shown);
    updateRemoteFirePoints(s, shown);
    mDummyElecShown = shown && (s.flags & kStatusElec) != 0;
    if (mDummyElecShown) {
        setElecDamageEffect();
        // procCoElecDamage's loop
        playRemotePlayerSfx(Z2SE_AL_ELEC_DAMAGE, static_cast<uint8_t>(PlayerSfxKind::SoundLevel),
                            0);
    }
    mDummyStatus = s;
}

// setFreezeEffect, with the wire's ice block bit for its proc check
void daDummyPlayer_c::updateRemoteFreezeEffect(const twili::RemoteStatusFx& s, bool shown) {
    static const Vec kScale = {1.0f, 1.8f, 1.0f};
    static const Vec kWolfScale = {1.0f, 1.0f, 1.5f};
    static const Vec kWolfOffset = {0.0f, 0.0f, -10.0f};
    if (!shown) {
        // Nothing thaws out of sight
        if (mDummyWasFrozen) {
            stopDrawParticle(field_0x3268);
        }
        mDummyWasFrozen = false;
        return;
    }
    const bool wolf = checkWolf() != 0;
    cXyz pos;
    if ((s.flags & twili::kStatusFrozen) != 0) {
        if ((s.flags & twili::kStatusIceBlock) != 0) {
            if (wolf) {
                mDoMtx_multVec(mpLinkModel->getAnmMtx(2), &kWolfOffset, &pos);
            } else {
                mDoMtx_multVecZero(mpLinkModel->getAnmMtx(1), &pos);
            }
            JPABaseEmitter* emitter =
                setEmitter(&field_0x3268, dPa_RM(ID_ZI_S_LK_FREEZ_A), &pos, &shape_angle);
            if (emitter != NULL) {
                emitter->setLocalScale(wolf ? kWolfScale : kScale);
            }
        }
        mDummyWasFrozen = true;
    } else if (mDummyWasFrozen) {
        mDoMtx_multVecZero(mpLinkModel->getAnmMtx(wolf ? 2 : 1), &pos);
        dComIfGp_particle_setColor(dPa_RM(ID_ZI_S_LK_FREEZEND_A), &pos, &tevStr, NULL, NULL,
                                   0.0f, 0xFF);
        dComIfGp_particle_setColor(dPa_RM(ID_ZI_S_LK_FREEZEND_B), &pos, &tevStr, NULL, NULL,
                                   0.0f, 0xFF);
        stopDrawParticle(field_0x3268);
        mDummyWasFrozen = false;
        mDummyThaws++;
    }
}

void daDummyPlayer_c::updateRemoteFirePoints(const twili::RemoteStatusFx& s, bool shown) {
    const u16 joints = bodyJointNum();
    bool lit = false;
    for (int i = 0; i < 4; i++) {
        firePointEff_c& e = field_0x32d8[i];
        const twili::RemoteFirePoint& w = s.fire[i];
        if (!shown || !w.active() || w.joint >= joints) {
            if (e.field_0x0 != 0) {
                releaseRemoteFirePoint(i);
            }
            mDummyFireSpent[i] = false;
            mDummyFireGen[i] = w.gen;
            continue;
        }
        if (w.gen != mDummyFireGen[i] || (e.field_0x0 == 0 && !mDummyFireSpent[i])) {
            if (e.field_0x0 != 0) {
                releaseRemoteFirePoint(i);
            }
            // initFirePointDamageEffectAll's slot setup.
            e.field_0x0 = 1;
            e.field_0x2 = w.joint;
            e.field_0x4 = 0;
            e.field_0x8 = 0;
            e.field_0x24 = cXyz::Zero;
            e.field_0x18.set(w.off[0], w.off[1], w.off[2]);
            mDoMtx_multVec(mpLinkModel->getAnmMtx(e.field_0x2), &e.field_0x18, &e.field_0xc);
            mDummyFireGen[i] = w.gen;
            mDummyFireSpent[i] = false;
        }
        lit = true;
    }
    if (lit || field_0x2fcb != 0) {
        // Plays Z2SE_AL_BURNING as well.
        ScopedDummyLinkAudioPtr audio(&mZ2Link);
        setFirePointDamageEffect();
    }
    // A slot whose flames ran out was cleared by setFirePointDamageEffect
    for (int i = 0; i < 4; i++) {
        if (field_0x32d8[i].field_0x0 == 0 && shown && s.fire[i].active() &&
            s.fire[i].joint < joints)
        {
            mDummyFireSpent[i] = true;
        }
    }
}

void daDummyPlayer_c::releaseRemoteFirePoint(int slot) {
    static const Vec kNoTrace = {0.0f, 0.0f, 0.0f};
    firePointEff_c& e = field_0x32d8[slot];
    for (u32 id : {e.field_0x4, e.field_0x8}) {
        if (JPABaseEmitter* emitter = dComIfGp_particle_getEmitter(id)) {
            emitter->setUserWork(reinterpret_cast<uintptr_t>(&kNoTrace));
        }
    }
    clearFirePointDamageEffect(slot);
}

void daDummyPlayer_c::updateRemoteShieldBurn(const twili::RemoteStatusFx& s, bool shown) {
    if (s.shieldBurnOutSeq != mDummyShieldBurnOutSeq) {
        mDummyShieldBurnOutSeq = s.shieldBurnOutSeq;
        if (shown && mDummyShieldBurnMtxValid) {
            // setWoodShieldBurnOutEffect, where the shield was on its last burning tick
            static const u16 kBurstNames[] = {ID_ZI_J_LK_SH_BURN_C, ID_ZI_J_LK_SH_BURN_D};
            for (u16 name : kBurstNames) {
                JPABaseEmitter* emitter = dComIfGp_particle_setColor(
                    name, &current.pos, &tevStr, NULL, NULL, 0.0f, -1);
                if (emitter != NULL) {
                    emitter->setGlobalRTMatrix(mDummyShieldBurnMtx);
                }
            }
            mDummyShieldBurnOuts++;
        }
    }
    if (shown && s.shieldBurn != 0 && !checkWolf()) {
        field_0x2fcb = s.shieldBurn;
        setWoodShieldBurnEffect();
        mDoMtx_copy(mShieldModel->getBaseTRMtx(), mDummyShieldBurnMtx);
        mDummyShieldBurnMtxValid = true;
    } else {
        // Stops the flames and zeroes field_0x2fcb
        clearWoodShieldBurnEffect();
        mDummyShieldBurnMtxValid = false;
    }
}

void daDummyPlayer_c::clearRemoteStatus() {
    for (int i = 0; i < 4; i++) {
        if (field_0x32d8[i].field_0x0 != 0) {
            releaseRemoteFirePoint(i);
        }
        mDummyFireSpent[i] = false;
    }
    clearWoodShieldBurnEffect();
    mDummyShieldBurnMtxValid = false;
    if (mDummyWasFrozen) {
        stopDrawParticle(field_0x3268);
    }
    mDummyWasFrozen = false;
    mDummyElecShown = false;
    offNoResetFlg1(FLG1_FREEZE_DAMAGE);
    mDamageTimer = 0;
    mDamageColorTime = 0;
    mIceDamageWaitTimer = 0;
}

bool daDummyPlayer_c::applyRemoteState(const LinkPuppetState& state) {
    if (!mpLinkModel) {
        return false;
    }
    const bool trace = !mDummyShellReady;
    traceInitialApply(mDummyClientId, trace, "applyRemoteState start");

    old.pos = current.pos;
    old.angle = shape_angle;

    current.pos.x = state.posX;
    current.pos.y = state.posY;
    current.pos.z = state.posZ;
    current.angle.x = state.angleX;
    current.angle.y = state.angleY;
    current.angle.z = state.angleZ;
    shape_angle.x = state.shapeAngleX;
    shape_angle.y = state.shapeAngleY;
    shape_angle.z = state.shapeAngleZ;
    mBodyAngle.x = state.bodyAngleX;
    mBodyAngle.y = state.bodyAngleY;
    mBodyAngle.z = state.bodyAngleZ;
    field_0x30c8 = state.bodyTwistY;
    field_0x306c = shape_angle.y + mBodyAngle.y;
    mMoveAngle = current.angle.y;
    mPrevAngleY = shape_angle.y;

    applyRemoteForm(state.transformStatus != 0, state.clothesItem);
    const bool wolf = checkWolf() != 0;
    mDummyRider = !wolf && state.horse.present() ? state.horse.rider
                                                  : twili::RemoteHorseRider{};
    bool onHorse = false;
    if (mDummyRider.active) {
        cXyz seat;
        if (horse::riderSeat(mDummyClientId, mDummyRider, seat)) {
            current.pos = seat;
            onHorse = true;
        }
    }
    LinkPuppetState gear = state;
    if (wolf) {
        gear.equipItem = (state.wolfFx.flags & twili::kWolfFxDome)
                             ? twili::wolffx::kLockDomeItem
                             : dItemNo_NONE_e;
        gear.cutType = daPy_py_c::CUT_TYPE_NONE;
        gear.swordBlurActive = false;
        gear.swordChargeActive = false;
        gear.shieldInHand = false;
        gear.itemAmmoLoaded = false;
        gear.itemProjectileType = DUMMY_PROJECTILE_NONE;
        gear.visFlags &= twili::kVisNoDraw;
    } else {
        applyRemoteClothes(state.clothesItem);
    }
    updateRemoteEquipment(gear);
    ScopedSelectEquip equip(mDummyClothesItem, mDummySwordItem, mDummyShieldItem);
    mDummyColorR = state.colorR;
    mDummyColorG = state.colorG;
    mDummyColorB = state.colorB;
    updateRemoteRecolor();
    updateRemoteMagicArmor(state);
    for (int i = 0; i < 2; i++) {
        field_0x32a0[i].r = state.waterDropColors[i][0];
        field_0x32a0[i].g = state.waterDropColors[i][1];
        field_0x32a0[i].b = state.waterDropColors[i][2];
        field_0x32a0[i].a = state.waterDropColors[i][3];
        field_0x32b0[i].r = state.swordUpColors[i][0];
        field_0x32b0[i].g = state.swordUpColors[i][1];
        field_0x32b0[i].b = state.swordUpColors[i][2];
        field_0x32b0[i].a = state.swordUpColors[i][3];
    }
    mCutType = gear.cutType;
    if (!wolf) {
        mLeftHandIndex = sanitizeLeftHandIndex(state.leftHandIndex);
        mRightHandIndex = sanitizeRightHandIndex(state.rightHandIndex);
        if (mEquipItem == kSwordEquipItem && mLeftHandIndex == 0xFE) {
            mLeftHandIndex = 2;
        }
        if ((state.shieldInHand || mEquipItem == kSwordEquipItem) && mRightHandIndex == 0xFE) {
            mRightHandIndex = 6;
        }
        mLeftHandIndex = sanitizeHandIndexForModel(mLeftHandIndex, mpLinkHandModel);
        mRightHandIndex = sanitizeHandIndexForModel(mRightHandIndex, mpLinkHandModel);
        applyRemoteItemPresentation(state);
    }

    if (gear.shieldInHand) {
        onNoResetFlg0(FLG0_UNK_2);
    } else {
        offNoResetFlg0(FLG0_UNK_2);
    }

    traceInitialApply(mDummyClientId, trace, "applyRemoteState setRemoteAnimations");
    if (!setRemoteAnimations(state)) {
        traceInitialApply(mDummyClientId, trace, "applyRemoteState animationsFailed");
        return false;
    }
    traceInitialApply(mDummyClientId, trace, "applyRemoteState animations ready");

    if (!field_0x1f20 || !field_0x1f24 || !field_0x2060 || !mSwordModel || !mSheathModel ||
        !mShieldModel || !mpLinkFaceModel || !mpLinkHatModel)
    {
        TwiliLog.warn("[dummy {}] missing shell model/calc pointers lowerCalc={} upperCalc={} "
                     "oldFrame={} sword={} sheath={} shield={} face={} hat={}",
                     mDummyClientId, (void*)field_0x1f20, (void*)field_0x1f24,
                     (void*)field_0x2060, (void*)mSwordModel, (void*)mSheathModel,
                     (void*)mShieldModel, (void*)mpLinkFaceModel, (void*)mpLinkHatModel);
        return false;
    }

    mSwordChangeWaitTimer = 0;
    mShieldChangeWaitTimer = 0;
    traceInitialApply(mDummyClientId, trace, "applyRemoteState setMatrix");
    setMatrix();
    mSinkShapeOffset = state.status.sinkOffset;
    if (mSinkShapeOffset != 0.0f) {
        setMatrixOffset(&mSinkShapeOffset, mSinkShapeOffset);
    }
    field_0x2fb6 = state.upperBlendMode;
    field_0x3444 = clampRatio(state.upperBlendRatio);
    traceInitialApply(mDummyClientId, trace, "applyRemoteState modelCalc");
    modelCalcRemoteBody(state.attentionLock);
    if (onHorse) {
        // setHorseStirrup's order
        horse::applyRider(mDummyClientId, *this, state.horse);
    }
    traceInitialApply(mDummyClientId, trace, "applyRemoteState setBodyPartPos");
    setBodyPartPos();
    updateRemoteGround();
    traceInitialApply(mDummyClientId, trace, "applyRemoteState updateItemMatrices");
    if (wolf) {
        if (mHeldItemModel && mEquipItem != twili::wolffx::kLockDomeItem) {
            TwiliLog.warn("[dummy {}] wolf held item {} dropped", mDummyClientId, mEquipItem);
            clearRemoteHeldItemModel();
        }
        // The dome's scale (setWolfItemMatrix)
        mSearchBallScale = mDummyHeldItem == twili::wolffx::kLockDomeItem
                               ? state.wolfFx.domeRadius
                               : 0.0f;
        setWolfItemMatrix();
        // After modelCalcRemoteBody
        mDummyMidna.update(state.midna, mpLinkModel, state.frameAlpha, isBodyShown());
    } else {
        // Off the wolf's back she outlasts the wolf body (a transformation back to human).
        if (state.midna.mode == twili::kMidnaApart) {
            mDummyMidna.update(state.midna, nullptr, state.frameAlpha, isBodyShown());
        } else {
            mDummyMidna.deactivate();
        }
        updateRemoteItemMatrices(state);
    }
    traceInitialApply(mDummyClientId, trace, "applyRemoteState setAttentionPos");
    setAttentionPos();
    traceInitialApply(mDummyClientId, trace, "applyRemoteState updateAudio");
    updateRemoteAudio(gear);
    traceInitialApply(mDummyClientId, trace, "applyRemoteState updateSwordEffects");
    updateRemoteSwordEffects(gear);
    updateRemoteWolfFx(state);
    fopAcM_SetMtx(this, mpLinkModel->getBaseTRMtx());
    traceInitialApply(mDummyClientId, trace, "applyRemoteState complete");
    return true;
}

static void daDummyPlayer_pvpTgHitCallback(fopAc_ac_c* tgActor, dCcD_GObjInf* tg,
                                           fopAc_ac_c* atActor, dCcD_GObjInf* at) {
    if (isDummyPlayer(tgActor)) {
        static_cast<daDummyPlayer_c*>(tgActor)->onPvpTgHit(tg, atActor, at);
    }
}

// playerInit's TG setup (d_a_alink.cpp) with the TG-only sources.
void daDummyPlayer_c::initRemoteHurtbox() {
    for (dCcD_Cyl& cyl : mTgCyls) {
        cyl.Set(l_dummyTgCylSrc);
        cyl.SetStts(&mCcStts);
        cyl.SetTgShieldFrontRangeYAngle(&field_0x306c);  // applyRemoteState keeps it current
        cyl.SetTgHitCallback(daDummyPlayer_pvpTgHitCallback);
    }
    mTgCyls[1].SetH(90.0f);
    mTgCyls[2].SetH(90.0f);
    mAtSph.Set(l_dummyTgSphSrc);
    mAtSph.SetStts(&mCcStts);
    mAtSph.SetTgHitCallback(daDummyPlayer_pvpTgHitCallback);
}

// After applyRemoteState, which placed the body (setBodyPartPos) the TG positions come from.
void daDummyPlayer_c::updateRemoteHurtbox(const twili::Client& client,
                                          const LinkPuppetState& state) {
    // As setCollision does first
    mCcStts.Move();
    // Z-targetable as an opponent (the Bokoblins' distance entry)
    if (twili::pvp::lockOnEnabled(client) && isBodyShown()) {
        attention_info.flags |= fopAc_AttnFlag_BATTLE_e;
        attention_info.distances[fopAc_attn_BATTLE_e] = 3;
    }
    if (!twili::pvp::hurtboxEnabled(client) || !isBodyShown() || state.modelSwap) {
        return;
    }
    const bool wolf = checkWolf() != 0;
    // The pose shown, not the newest one
    const bool guard = !wolf && (state.visFlags & twili::kVisGuard) != 0;
    // setCollision's shield flags.
    for (dCcD_Cyl& cyl : mTgCyls) {
        cyl.SetTgHitMark(CcG_Tg_UNK_MARK_6);
        cyl.OffTgSpShield();
        if (guard) {
            cyl.OnTgShield();
        } else {
            cyl.OffTgShield();
        }
        if (wolf) {
            cyl.OffTgShieldFrontRange();
        } else {
            cyl.OnTgShieldFrontRange();
        }
    }
    if (wolf) {
        setWolfCollisionPos();
    } else {
        setCollisionPos();
    }
    for (dCcD_Cyl& cyl : mTgCyls) {
        cyl.OffAtSetBit();
        cyl.OffCoSetBit();
        cyl.OnTgSetBit();
        dComIfG_Ccsp()->Set(&cyl);
    }
    if (wolf) {
        mAtSph.OffAtSetBit();
        mAtSph.OffCoSetBit();
        mAtSph.OnTgSetBit();
        dComIfG_Ccsp()->Set(&mAtSph);
    }
    mDummyHurtboxLive = true;
    mDummyHurtboxGuard = guard;
}

// Runs inside dComIfG_Ccsp()->Move(), while a projectile that hit is still alive.
void daDummyPlayer_c::onPvpTgHit(dCcD_GObjInf* tg, fopAc_ac_c* atActor, dCcD_GObjInf* at) {
    twili::pvp::HitReport hit;
    if (!mDummyHurtboxLive ||
        !twili::pvp::classifyLocalAttack(atActor, at, tg, this, hit))
    {
        return;
    }
    hit.viewSeq = static_cast<uint32_t>(std::max(mDummyPlayout.renderSeq, mDummyPlayout.holdSeq));
    twili::Session::instance().queuePvpHit(mDummyClientId, hit);
}

cPhs_Step daDummyPlayer_c::create() {
    fopAcM_ct(this, daDummyPlayer_c);
    mDummyClientId = (uint32_t)fopAcM_GetParam(this);
    restoreLocalLinkAudioPtr();
    if (const cPhs_Step step = loadPrivateArchives(); step != cPhs_COMPLEATE_e) {
        return step;
    }
    TwiliLog.info("[dummy {}] create begin", mDummyClientId);
    fopAcM_setStageLayer(this);

    LinkPuppetState initialState;
    const auto& clients = twili::Session::instance().clients();
    const auto it = clients.find(mDummyClientId);
    if (it != clients.end()) {
        const twili::RemotePoseEval ev =
            it->second.pose.advance(mDummyPlayout, initialState);
        if (!ev.valid) {
            initialState = makeLinkPuppetState(it->second);
        }
        // The colour is client state, not part of the pose
        initialState.colorR = it->second.colorR;
        initialState.colorG = it->second.colorG;
        initialState.colorB = it->second.colorB;
        // Spawned in its form
        mDummyWasWolf = initialState.transformStatus != 0;
        current.pos.set(initialState.posX, initialState.posY, initialState.posZ);
        current.angle.set(initialState.angleX, initialState.angleY, initialState.angleZ);
        shape_angle.set(initialState.shapeAngleX, initialState.shapeAngleY,
                        initialState.shapeAngleZ);
    }

    if (!fopAcM_entrySolidHeap(this, daDummyPlayer_createHeap,
                               kDummyLinkHeapSize | kDummyLinkHeapFlags))
    {
        TwiliLog.warn("[dummy {}] create: entrySolidHeap failed", mDummyClientId);
        return cPhs_ERROR_e;
    }
    mDummyItemFx.init();
    if (it != clients.end()) {
        // After createHeap
        updateRemoteHidden(it->second, initialState);
    }

    if (!initializeShell(initialState)) {
        TwiliLog.warn("[dummy {}] create: shell initialization failed", mDummyClientId);
        return cPhs_ERROR_e;
    }

    fopAcM_SetMtx(this, mpLinkModel->getBaseTRMtx());
    fopAcM_setCullSizeBox(this, -1000.0f, -500.0f, -1000.0f, 1000.0f, 900.0f,
                          1000.0f);
    TwiliLog.info("[dummy {}] create complete", mDummyClientId);
    return cPhs_COMPLEATE_e;
}

int daDummyPlayer_c::execute() {
    const auto& clients = twili::Session::instance().clients();
    auto it = clients.find(mDummyClientId);
    if (it == clients.end()) {
        fopAcM_delete(this);
        return TRUE;
    }
    // Registered again below only when this tick gets that far.
    mDummyHurtboxLive = false;
    mDummyHurtboxGuard = false;
    attention_info.flags &= ~fopAc_AttnFlag_BATTLE_e;

    if (!mDummyShellReady) {
        return TRUE;
    }

    const auto& client = it->second;
    const char* myStage = dComIfGp_getStartStageName();
    const int8_t myLayer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    if (!myStage || std::strncmp(client.stageName, myStage, 8) != 0 ||
        client.layerNo != myLayer)
    {
        mDummyItemFx.stopAll();
        return TRUE;
    }

    LinkPuppetState state;
    const twili::RemotePoseEval ev = client.pose.advance(mDummyPlayout, state);
    if (!ev.valid) {
        // The client entry was rebuilt (ALL_CLIENT_STATE) and holds no pose yet
        mDummyItemFx.stopAll();
        return TRUE;
    }
    const double shownSeq = (std::max)(mDummyPlayout.renderSeq, mDummyPlayout.holdSeq);
    // The color is client state, not part of the (delayed) pose stream.
    state.colorR = client.colorR;
    state.colorG = client.colorG;
    state.colorB = client.colorB;

    const bool wolf = state.transformStatus != 0;
    updateRemoteHidden(client, state);
    if (ev.snapped || wolf != mDummyWasWolf) {
        resetRemoteContinuity();
    }
    mDummyWasWolf = wolf;
    // Its horse first, as daHorse_c executes before daAlink_c
    horse::applyPuppetPose(mDummyClientId, state.horse, state.frameAlpha, ev.snapped);
    if (wolf && !mpDummyWolfModel) {
        // No wolf body to play the wolf clips on (createHeap could not build it)
        current.pos.set(state.posX, state.posY, state.posZ);
        current.angle.set(state.angleX, state.angleY, state.angleZ);
        shape_angle.set(state.shapeAngleX, state.shapeAngleY, state.shapeAngleZ);
        attention_info.position = current.pos;
        updateRemoteAudio(state);
        updateRemoteSwordEffects(state);
        clearRemoteStatus();
        clearRemoteWolfFx();
        mDummyItemFx.update(*this, state, shownSeq, client.itemFxEvents, ev.snapped, false);
        playQueuedSfx(client, shownSeq, ev.snapped);
        return TRUE;
    }

    const bool applied = applyRemoteState(state);
    updateRemoteTransformFx(client, state, shownSeq);
    if (applied) {
        // After the matrices
        updateRemoteStatus(state);
        updateRemoteHurtbox(client, state);
    } else {
        clearRemoteStatus();
        clearRemoteWolfFx();
    }
    // Whatever the remote's body does
    mDummyItemFx.update(*this, state, shownSeq, client.itemFxEvents, ev.snapped, !isHidden());
    playQueuedSfx(client, shownSeq, ev.snapped);
    if (ev.snapped) {
        // Nothing may streak across the jump
        old.pos = current.pos;
        old.angle = shape_angle;
        if (!mDummyHidden) {
            interp::requestPresentationSync();
        }
    }
    return TRUE;
}

int daDummyPlayer_c::draw() {
    const auto& clients = twili::Session::instance().clients();
    auto it = clients.find(mDummyClientId);
    // An early return, not fopAcStts_NODRAW_e
    if (it == clients.end() || !mDummyShellReady || isHidden()) {
        return TRUE;
    }

    const auto& client = it->second;
    const char* myStage = dComIfGp_getStartStageName();
    if (!myStage || std::strncmp(client.stageName, myStage, 8) != 0) {
        if (!mDummyDrawLogged) {
            TwiliLog.debug("[dummy {}] draw: stage mismatch (client='{}' me='{}')",
                          mDummyClientId, client.stageName, myStage ? myStage : "null");
            mDummyDrawLogged = true;
        }
        return TRUE;
    }

    const int8_t myLayer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    if (client.layerNo != myLayer) {
        if (!mDummyDrawLogged) {
            TwiliLog.debug("[dummy {}] draw: layer mismatch (client={} me={})",
                          mDummyClientId, (int)client.layerNo, (int)myLayer);
            mDummyDrawLogged = true;
        }
        return TRUE;
    }

    mDummyDrawLogged = false;
    // Its arrows, boomerang and bombs are not part of its body
    mDummyItemFx.draw(*this);
    // Whatever hides the remote's body there (a sky cannon, a boss door) hides it here too.
    if (mDummyVisFlags & twili::kVisNoDraw) {
        return TRUE;
    }
    if (mDummyTfPlan.silhouette) {
        // daAlink_c::draw's model swap branch
        if (mDummyTfSilhouette) {
            g_env_light.settingTevStruct(checkWolf() ? 9 : 10, &current.pos, &tevStr);
            initTevCustomColor();
            tevStr.TevColor.r = mDummyTfPlan.silhouetteTev;
            tevStr.TevColor.g = mDummyTfPlan.silhouetteTev;
            tevStr.TevColor.b = mDummyTfPlan.silhouetteTev;
            g_env_light.setLightTevColorType_MAJI(mpWlChangeModel, &tevStr);
            mDoExt_modelEntryDL(mpWlChangeModel);
        }
        return TRUE;
    }
    ScopedSelectEquip equip(mDummyClothesItem, mDummySwordItem, mDummyShieldItem);
    ScopedRemoteRupees rupees(mDummyArmorWorn, mDummyArmorBrkStatus == 0);
    ScopedCameraAttentionMask cameraMask(field_0x317c, 0x20 | 0x2);
    mDummyDrawCount++;
    int result;
    {
        // The body darkens (or brightens) with the remote's transformation.
        ScopedRemoteMetamorphose metamorphose(*this, mDummyTfPlan.bodyTev, mDummyTf.tev,
                                              field_0x347c);
        result = daAlink_c::draw();
    }
    if (checkWolf() || mDummyMidna.mode() == twili::kMidnaApart) {
        // daMidna_c::draw takes Link's colour while he is frozen or chilled.
        mDummyMidna.draw(tevStr, checkFreezeDamage() || mIceDamageWaitTimer != 0);
    }
    drawRemoteLoadedAmmo();
    drawRemoteShadow();
    return result;
}

void daDummyPlayer_c::drawRemoteLoadedAmmo() {
    if (mpDummyLoadedAmmoModel) {
        modelDraw(mpDummyLoadedAmmoModel, 0);
    }
}

// The ground under the dummy for its shadow.
void daDummyPlayer_c::updateRemoteGround() {
    // From the root joint (setBodyPartPos) down, so a foot sunk into a slope still finds it.
    cXyz start = field_0x3834;
    mObjGndChk.SetPos(&start);
    mDummyGroundY = dComIfG_Bgsp().GroundCross(&mObjGndChk);
}

void daDummyPlayer_c::drawRemoteShadow() {
    // None while its room is not loaded here.
    if (mDummyGroundY <= -G_CM3D_F_INF) {
        return;
    }

    // A rider adds itself to its horse's real shadow (daAlink_c::shadowDraw)
    const uint32_t horseShadow = mDummyRider.active ? horse::riddenShadowId(mDummyClientId) : 0;
    if (horseShadow != 0) {
        field_0x31a4 = horseShadow;
        dComIfGd_addRealShadow(field_0x31a4, mpLinkModel);
    } else {
        cXyz center = field_0x3834;
        field_0x31a4 = dComIfGd_setShadow(field_0x31a4, 0, mpLinkModel, &center, 800.0f, 0.0f,
                                          current.pos.y, mDummyGroundY, mObjGndChk, &tevStr, 0,
                                          1.0f, dDlst_shadowControl_c::getSimpleTex());
    }
    if (field_0x31a4 == 0) {
        return;
    }

    if (checkSwordDraw()) {
        dComIfGd_addRealShadow(field_0x31a4, mSwordModel);
        if (!checkWoodSwordEquip()) {
            dComIfGd_addRealShadow(field_0x31a4, mSheathModel);
        }
    }
    if (checkShieldDraw()) {
        dComIfGd_addRealShadow(field_0x31a4, mShieldModel);
    }
    if (checkWolf()) {
        mDummyMidna.addRealShadow(field_0x31a4);
        return;
    }
    if (checkItemDraw()) {
        dComIfGd_addRealShadow(field_0x31a4, mHeldItemModel);
    }
    if (checkNoResetFlg2(FLG2_UNK_1)) {
        dComIfGd_addRealShadow(field_0x31a4, mpKanteraModel);
    }
    if (checkEquipHeavyBoots()) {
        for (J3DModel* boot : mpLinkBootModels) {
            dComIfGd_addRealShadow(field_0x31a4, boot);
        }
    }
}

void daDummyPlayer_c::destroy() {
    if (!mDummyConstructed) {
        return;
    }
    clearRemoteStatus();
    clearRemoteWolfFx();
    mDummyItemFx.destroy();
    // The lantern's swing fire calls back into this actor (field_0x2f20) while it fades.
    if (JPABaseEmitter* flame = dComIfGp_particle_getEmitter(field_0x31c4)) {
        flame->setEmitterCallBackPtr(nullptr);
        flame->stopDrawParticle();
    }
    mZ2Link.deleteKantera();
    mZ2Link.deleteObject();
    restoreLocalLinkAudioPtr();
    if (mpHookSound != nullptr) {
        mpHookSound->deleteObject();
        mpHookSound = nullptr;
    }

    destroyDummyAnmHeaps();
    unmountPrivateArchives();
}

static void destroyAnmHeap(daPy_anmHeap_c& heap) {
    if (heap.mAnimeHeap != nullptr) {
        mDoExt_destroySolidHeap(heap.mAnimeHeap);
        heap.mAnimeHeap = nullptr;
    }
}

// initializeShell creates these from the game heap.
void daDummyPlayer_c::destroyDummyAnmHeaps() {
    destroyAnmHeap(mAnmHeap3);
    destroyAnmHeap(mAnmHeap4);
    for (int i = 0; i < kAnmPackCount; i++) {
        destroyAnmHeap(mUnderAnmHeap[i]);
        destroyAnmHeap(mUpperAnmHeap[i]);
    }
    destroyAnmHeap(mFaceBtpHeap);
    destroyAnmHeap(mFaceBtkHeap);
    destroyAnmHeap(mFaceBckHeap);
    destroyAnmHeap(mItemHeap[0]);
    destroyAnmHeap(mItemHeap[1]);
    destroyAnmHeap(mAnmHeap9);
    mDummyMidna.destroyHeaps();
}

void daDummyPlayer_c::unmountPrivateArchives() {
    mpDummyKmdlArchive = mpDummyMmdlArchive = mpDummyZmdlArchive = mpDummyBmdlArchive = nullptr;
    mpDummyAlinkArchive = mpDummyHylianShieldArchive = mpDummyOrdonShieldArchive = nullptr;
    mpDummyWoodShieldArchive = mpDummyWmdlArchive = nullptr;
    mDummyArchives.release();
}

namespace {

int daDummyPlayer_create(void* i_this) {
    return static_cast<daDummyPlayer_c*>(i_this)->create();
}

int daDummyPlayer_delete(void* i_this) {
    static_cast<daDummyPlayer_c*>(i_this)->destroy();
    return TRUE;
}

int daDummyPlayer_execute(void* i_this) {
    return static_cast<daDummyPlayer_c*>(i_this)->execute();
}

int daDummyPlayer_isDelete(void* i_this) {
    return !static_cast<daDummyPlayer_c*>(i_this)->privateArchivesBusy();
}

int daDummyPlayer_draw(void* i_this) {
    return static_cast<daDummyPlayer_c*>(i_this)->draw();
}

}  // namespace

// After Link's list (5) and most actors, so it reads this tick's local state.
const ActorProfileDesc g_dummyPlayerProfile = {
    .name = "TTlink",
    .priority_group = 9,
    .process_size = sizeof(daDummyPlayer_c),
    .draw_priority = fpcDwPi_ALINK_e,
    .status = fopAcStts_UNK_0x40000_e | fopAcStts_NOPAUSE_e,
    .group = fopAc_NPC_e,
    .cull_type = fopAc_CULLBOX_CUSTOM_e,
    .create_function = daDummyPlayer_create,
    .delete_function = daDummyPlayer_delete,
    .execute_function = daDummyPlayer_execute,
    .is_delete_function = daDummyPlayer_isDelete,
    .draw_function = daDummyPlayer_draw,
};

}  // namespace twili
