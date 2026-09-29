#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include "core/GameAccess.hpp"
#include "fx/ItemFx.hpp"

#include "Z2AudioLib/Z2LinkMgr.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/d_com_inf_game.h"
#include "d/d_particle_name.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_kankyo_mng.h"

namespace twili::hooks {

DEFINE_HOOK(&daAlink_c::changeCutReverseProc, LinkChangeCutReverseProc);
DEFINE_HOOK(&daAlink_c::setCutLargeJumpLandEffect, LinkSetCutLargeJumpLandEffect);
DEFINE_HOOK(&daAlink_c::setHookshotPos, LinkSetHookshotPos);
DEFINE_HOOK(&daAlink_c::setIronBallPos, LinkSetIronBallPos);
DEFINE_HOOK(&daAlink_c::loadModelDVD, LinkLoadModelDVD);
// Z2CreatureLink has two bases: MSVC cannot constant-initialize a DEFINE_HOOK record for it.
DEFINE_HOOK_SYMBOL("Z2CreatureLink::startHitItemSE",
    Z2SoundHandlePool*(Z2CreatureLink*, u32, u32, Z2SoundObjBase*, f32), LinkStartHitItemSE);
static_assert(std::is_same_v<decltype(&Z2CreatureLink::startHitItemSE),
    Z2SoundHandlePool* (Z2CreatureLink::*)(u32, u32, Z2SoundObjBase*, f32)>);
DEFINE_HOOK(&fopKyM_createWpillar, KankyoCreateWpillar);
// setBombArrowExplode is inlined into the arrow's execute: its NBOMB create is tapped there.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_arrow.cpp#daArrow_execute", int(daArrow_c*), ArrowExecute);
using FastCreateFn = fopAc_ac_c* (*)(s16, u32, const cXyz*, int, const csXyz*, const cXyz*, s8,
    createFunc, void* IF_DUSK_ARG(u32) IF_DUSK_ARG(u8));
DEFINE_HOOK(static_cast<FastCreateFn>(&fopAcM_fastCreate), ActorFastCreate);
// Header-inline wrappers: the mod has its own copies, so these resolve the game's by name.
DEFINE_HOOK_SYMBOL("?dComIfGp_particle_set@@YAPEAVJPABaseEmitter@@GPEBUcXyz@@PEBVcsXyz@@0@Z",
    JPABaseEmitter*(u16, const cXyz*, const csXyz*, const cXyz*), ParticleSetRot);
DEFINE_HOOK_SYMBOL(
    "?dComIfGp_particle_set@@YAPEAVJPABaseEmitter@@GPEBUcXyz@@PEBVdKy_tevstr_c@@PEBVcsXyz@@0@Z",
    JPABaseEmitter*(u16, const cXyz*, const dKy_tevstr_c*, const csXyz*, const cXyz*),
    ParticleSetTevRot);
DEFINE_HOOK_SYMBOL("?dComIfGp_particle_setPolyColor@@YAPEAVJPABaseEmitter@@GAEAVcBgS_PolyInfo@@"
                   "PEBUcXyz@@PEBVdKy_tevstr_c@@PEBVcsXyz@@1HPEAVdPa_levelEcallBack@@C1@Z",
    JPABaseEmitter*(u16, cBgS_PolyInfo&, const cXyz*, const dKy_tevstr_c*, const csXyz*,
        const cXyz*, int, dPa_levelEcallBack*, s8, const cXyz*),
    ParticleSetPolyColor);
DEFINE_HOOK_SYMBOL("?dComIfGp_setHitMark@@YAXGPEAVfopAc_ac_c@@PEBUcXyz@@PEBVcsXyz@@1I@Z",
    void(u16, fopAc_ac_c*, const cXyz*, const csXyz*, const cXyz*, u32), SetHitMark);

namespace {

using perf::Target;

// Dummies run these members to pose their own clawshot and ball: only the local player's count.
template <ScopeKind Kind>
HookAction onScopePre(ModContext*, void* args, void*, void*) {
    perf::count(Target::FxScope);
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link != nullptr && link == localLink()) {
        Scope::push(Kind, link);
    }
    return HOOK_CONTINUE;
}

template <ScopeKind Kind>
void onScopePost(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (Scope::owner(Kind) == link) {
        Scope::pop(Kind, link);
    }
}

void onParticleSetRotPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::ParticleSet);
    if (!Scope::inside(ScopeKind::CutReverse)) {
        return;
    }
    const u16 id = mods::arg<u16>(args, 0);
    const auto* pos = mods::arg<const cXyz*>(args, 1);
    const auto* rot = mods::arg<const csXyz*>(args, 2);
    if ((id == ID_ZI_J_COLHIT_KIKUZU || id == ID_ZI_J_COLHIT_ICE || id == ID_ZI_J_COLHIT_HIBANA) &&
        pos != nullptr && rot != nullptr)
    {
        itemfx::noteParticle(id, 1, *pos, rot->x, rot->y);
    }
}

// The landing sets six particles; one note stands for all of them.
void onParticleSetTevRotPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::ParticleSet);
    if (!Scope::inside(ScopeKind::JumpLand)) {
        return;
    }
    const u16 id = mods::arg<u16>(args, 0);
    const auto* pos = mods::arg<const cXyz*>(args, 1);
    const auto* rot = mods::arg<const csXyz*>(args, 3);
    if (id == ID_ZI_J_LK_DJGIRI_A && pos != nullptr && rot != nullptr) {
        itemfx::noteParticle(id, 6, *pos, rot->x, rot->y);
    }
}

void onParticleSetPolyColorPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::PolyColor);
    if (!Scope::inside(ScopeKind::Hookshot)) {
        return;
    }
    const auto* pos = mods::arg<const cXyz*>(args, 2);
    const auto* rot = mods::arg<const csXyz*>(args, 4);
    const auto* scale = mods::arg<const cXyz*>(args, 5);
    if (pos != nullptr && rot != nullptr) {
        itemfx::noteParticle(mods::arg<u16>(args, 0), 1, *pos, rot->x, rot->y,
            scale != nullptr ? scale->x : 1.0f);
    }
}

void onSetHitMarkPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::HitMark);
    const ScopeKind kind = Scope::top().kind;
    if (kind != ScopeKind::Hookshot && kind != ScopeKind::IronBall) {
        return;
    }
    const u16 type = mods::arg<u16>(args, 0);
    const auto* pos = mods::arg<const cXyz*>(args, 2);
    const auto* rot = mods::arg<const csXyz*>(args, 3);
    if (type == 9 && pos != nullptr && rot != nullptr) {
        itemfx::noteHitMark(type, *pos, rot->x, rot->y);
    }
}

// The sound plays at the hit point, the clawshot tip or the ball.
void onStartHitItemSEPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::HitItemSe);
    const ScopeEntry scope = Scope::top();
    if (scope.kind != ScopeKind::CutReverse && scope.kind != ScopeKind::Hookshot &&
        scope.kind != ScopeKind::IronBall)
    {
        return;
    }
    auto* link = static_cast<daAlink_c*>(const_cast<void*>(scope.owner));
    if (mods::arg<Z2CreatureLink*>(args, 0) != &link->mZ2Link) {
        return;
    }
    const u32 id = mods::arg<u32>(args, 1);
    const u32 mapInfo = mods::arg<u32>(args, 2);
    switch (scope.kind) {
    case ScopeKind::CutReverse:
        itemfx::noteSound(id, *link->mLinkLinChk.GetCrossP(), mapInfo);
        break;
    case ScopeKind::Hookshot:
        itemfx::noteSound(id, link->mHookshotTopPos, mapInfo);
        break;
    case ScopeKind::IronBall:
        if (id == Z2SE_HIT_HAMMER) {
            itemfx::noteSound(id, link->mIronBallCenterPos, mapInfo);
        }
        break;
    default:
        break;
    }
}

void onCreateWpillarPost(ModContext*, void* args, void*, void*) {
    perf::count(Target::Wpillar);
    const ScopeKind kind = Scope::top().kind;
    if (kind != ScopeKind::Hookshot && kind != ScopeKind::IronBall) {
        return;
    }
    const auto* pos = mods::arg<const cXyz*>(args, 0);
    if (pos != nullptr) {
        itemfx::noteWater(*pos, mods::arg<f32>(args, 1));
    }
}

HookAction onArrowExecutePre(ModContext*, void* args, void*, void*) {
    perf::count(Target::BombArrow);
    auto* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow != nullptr && arrow->mArrowType == daArrow_c::ARROW_TYPE_BOMB) {
        Scope::push(ScopeKind::BombArrow, arrow);
    }
    return HOOK_CONTINUE;
}

void onArrowExecutePost(ModContext*, void* args, void*, void*) {
    auto* arrow = mods::arg<daArrow_c*>(args, 0);
    if (Scope::owner(ScopeKind::BombArrow) == arrow) {
        Scope::pop(ScopeKind::BombArrow, arrow);
    }
}

// setBombArrowExplode's NBOMB is no PLAYER_MAKE bomb: peers' copies and PvP learn it is ours here.
void onFastCreatePost(ModContext*, void* args, void*, void*) {
    perf::count(Target::FastCreate);
    const auto* arrow = static_cast<const daArrow_c*>(Scope::owner(ScopeKind::BombArrow));
    if (arrow == nullptr || mods::arg<s16>(args, 0) != fpcNm_NBOMB_e ||
        mods::arg<u32>(args, 1) != 0)
    {
        return;
    }
    const auto* pos = mods::arg<const cXyz*>(args, 2);
    if (pos != nullptr) {
        itemfx::noteBombArrowExplode(*pos, arrow->field_0x945 != 0);
    }
}

// The Link whose clothes archive this loadModelDVD call frees.
daAlink_c* s_freeingLink = nullptr;

// Wait while the previous clothes archive still mounts (its heap is in use); timer stays 3.
HookAction onLoadModelDVDPre(ModContext*, void* args, void* retval, void*) {
    perf::count(Target::LoadModel);
    auto* link = mods::arg<daAlink_c*>(args, 0);
    s_freeingLink = nullptr;
    if (link->mClothesChangeWaitTimer != 3 || link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_280000)) {
        return HOOK_CONTINUE;
    }
    if (link->mPhaseReq.id == 1 && dComIfG_syncObjectRes(link->mArcName) > 0) {
        *static_cast<int*>(retval) = 0;
        return HOOK_SKIP_ORIGINAL;
    }
    s_freeingLink = link;
    return HOOK_CONTINUE;
}

// field_0x06ec points into the freed body; null it (Ordon clothes -> Zora armor crash).
void onLoadModelDVDPost(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (s_freeingLink == link) {
        link->field_0x06ec = nullptr;
    }
    s_freeingLink = nullptr;
}

template <class Entry>
ModResult addScope(HookPreFn pre, HookPostFn post, const char* what, std::string& error) {
    ModResult result = addPre<Entry>(pre, kDefault, what, error);
    if (result == MOD_OK) {
        result = addPost<Entry>(post, kDefault, what, error);
    }
    return result;
}

}  // namespace

const char* perf::targetName(Target target) {
    static constexpr const char* kNames[] = {"daSus_c::check", "daAlink_c::execute", "Link SFX",
        "fpcM_Management", "dComIfGp_particle_set", "dComIfGp_particle_setPolyColor",
        "dComIfGp_setHitMark", "startHitItemSE", "fopKyM_createWpillar", "effect scopes",
        "loadModelDVD", "arrow execute", "fopAcM_fastCreate", "checkDamageAction",
        "dCcD_GObjInf::GetTgHitGObj"};
    static_assert(std::size(kNames) == static_cast<size_t>(Target::Count));
    return kNames[static_cast<size_t>(target)];
}

// Crash protections for everyone, so they go with the core group.
ModResult installClothes(std::string& error) {
    return addScope<LinkLoadModelDVD>(
        onLoadModelDVDPre, onLoadModelDVDPost, "daAlink_c::loadModelDVD", error);
}

ModResult installFx(std::string& error) {
    const ModResult results[] = {
        addScope<LinkChangeCutReverseProc>(onScopePre<ScopeKind::CutReverse>,
            onScopePost<ScopeKind::CutReverse>, "changeCutReverseProc", error),
        addScope<LinkSetCutLargeJumpLandEffect>(onScopePre<ScopeKind::JumpLand>,
            onScopePost<ScopeKind::JumpLand>, "setCutLargeJumpLandEffect", error),
        addScope<LinkSetHookshotPos>(onScopePre<ScopeKind::Hookshot>,
            onScopePost<ScopeKind::Hookshot>, "setHookshotPos", error),
        addScope<LinkSetIronBallPos>(onScopePre<ScopeKind::IronBall>,
            onScopePost<ScopeKind::IronBall>, "setIronBallPos", error),
        addPost<ParticleSetRot>(onParticleSetRotPost, kDefault, "dComIfGp_particle_set", error),
        addPost<ParticleSetTevRot>(
            onParticleSetTevRotPost, kDefault, "dComIfGp_particle_set (tev)", error),
        addPost<ParticleSetPolyColor>(
            onParticleSetPolyColorPost, kDefault, "dComIfGp_particle_setPolyColor", error),
        addPost<SetHitMark>(onSetHitMarkPost, kDefault, "dComIfGp_setHitMark", error),
        addPost<LinkStartHitItemSE>(onStartHitItemSEPost, kDefault, "startHitItemSE", error),
        addPost<KankyoCreateWpillar>(onCreateWpillarPost, kDefault, "fopKyM_createWpillar", error),
        addScope<ArrowExecute>(onArrowExecutePre, onArrowExecutePost, "daArrow_execute", error),
        addPost<ActorFastCreate>(onFastCreatePost, kDefault, "fopAcM_fastCreate", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
