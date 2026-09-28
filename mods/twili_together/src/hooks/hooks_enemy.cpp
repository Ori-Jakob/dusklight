#include "hooks/Hooks.hpp"

#include "core/Session.hpp"
#include "enemy/EnemyScaling.hpp"
#include "enemy/EnemySync.hpp"

#include "m_Do/m_Do_ext.h"  // the enemy headers are not self-contained

#include "SSystem/SComponent/c_phase.h"
#include "d/actor/d_a_e_bs.h"
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_e_s1.h"
#include "f_op/f_op_actor_mng.h"

namespace twili::hooks {

DEFINE_HOOK_SYMBOL("src/f_op/f_op_actor.cpp#fopAc_Create", int(void*), ActorCreate);
DEFINE_HOOK_SYMBOL("src/f_op/f_op_actor.cpp#fopAc_Delete", int(void*), ActorDelete);
DEFINE_HOOK(static_cast<s32 (*)(fopAc_ac_c*)>(&fopAcM_delete), ActorDeleteRequest);
DEFINE_HOOK(static_cast<s32 (*)(fpc_ProcID)>(&fopAcM_delete), ActorDeleteRequestId);
DEFINE_HOOK(&fopAcM_createDisappear, ActorCreateDisappear);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_bs.cpp#damage_check", void(e_bs_class*), EbsDamageCheck);
DEFINE_HOOK(&daE_OC_c::executeFallDead, EocExecuteFallDead);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_s1.cpp#all_fail", void(e_s1_class*), Es1AllFail);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_s1.cpp#e_s1_shout", void(e_s1_class*), Es1Shout);

namespace {

const e_s1_class* s_shoutReset = nullptr;

// The append is freed right after this returns.
void onActorCreatePost(ModContext*, void* args, void* retval, void*) {
    if (!Session::active() || *static_cast<int*>(retval) != cPhs_COMPLEATE_e) {
        return;
    }
    auto* actor = static_cast<fopAc_ac_c*>(mods::arg<void*>(args, 0));
    enemy_scaling::onActorCreated(actor);
    enemy_sync::onActorCreated(actor);
}

void onActorDeletePost(ModContext*, void* args, void* retval, void*) {
    if (!Session::active() || *static_cast<int*>(retval) != 1) {
        return;
    }
    auto* actor = static_cast<fopAc_ac_c*>(mods::arg<void*>(args, 0));
    enemy_scaling::onActorDeleted(actor);
    enemy_sync::onActorDeleted(actor);
}

// Deaths without a puff delete from inside these scopes (B2, B3).
HookAction onDeleteRequestPre(ModContext*, void* args, void*, void*) {
    if (!Session::active()) {
        return HOOK_CONTINUE;
    }
    auto* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (actor != nullptr && Scope::owner(ScopeKind::EnemyDamage) == actor) {
        enemy_sync::markDefeated(actor);
    }
    enemy_sync::onDeleteRequest(actor);
    return HOOK_CONTINUE;
}

HookAction onDeleteRequestIdPre(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        enemy_sync::onDeleteRequest(mods::arg<fpc_ProcID>(args, 0));
    }
    return HOOK_CONTINUE;
}

HookAction onCreateDisappearPre(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        enemy_sync::onDisappear(
            mods::arg<const fopAc_ac_c*>(args, 0), mods::arg<u8>(args, 2), mods::arg<u8>(args, 3));
    }
    return HOOK_CONTINUE;
}

HookAction onEbsDamageCheckPre(ModContext*, void* args, void*, void*) {
    Scope::push(ScopeKind::EnemyDamage, &mods::arg<e_bs_class*>(args, 0)->enemy);
    return HOOK_CONTINUE;
}

void onEbsDamageCheckPost(ModContext*, void* args, void*, void*) {
    Scope::pop(ScopeKind::EnemyDamage, &mods::arg<e_bs_class*>(args, 0)->enemy);
}

HookAction onEocFallDeadPre(ModContext*, void* args, void*, void*) {
    Scope::push(ScopeKind::EnemyDamage, static_cast<fopAc_ac_c*>(mods::arg<daE_OC_c*>(args, 0)));
    return HOOK_CONTINUE;
}

void onEocFallDeadPost(ModContext*, void* args, void*, void*) {
    Scope::pop(ScopeKind::EnemyDamage, static_cast<fopAc_ac_c*>(mods::arg<daE_OC_c*>(args, 0)));
}

void onEs1AllFailPost(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        enemy_sync::onShadowBeastGroupDown(mods::arg<e_s1_class*>(args, 0));
    }
}

// Mode 0 resets health to a fixed 50, maybe below the scaled health.
HookAction onEs1ShoutPre(ModContext*, void* args, void*, void*) {
    auto* s1 = mods::arg<e_s1_class*>(args, 0);
    s_shoutReset = s1->mMode == 0 ? s1 : nullptr;
    return HOOK_CONTINUE;
}

void onEs1ShoutPost(ModContext*, void* args, void*, void*) {
    auto* s1 = mods::arg<e_s1_class*>(args, 0);
    if (s_shoutReset == s1 && Session::active() && s1->health == 50) {
        enemy_scaling::onHealthReset(s1);
    }
    s_shoutReset = nullptr;
}

}  // namespace

ModResult installEnemy(std::string& error) {
    const ModResult results[] = {
        addPost<ActorCreate>(onActorCreatePost, kDefault, "fopAc_Create", error),
        addPost<ActorDelete>(onActorDeletePost, kDefault, "fopAc_Delete", error),
        addPre<ActorDeleteRequest>(onDeleteRequestPre, kObserve, "fopAcM_delete", error),
        addPre<ActorDeleteRequestId>(onDeleteRequestIdPre, kObserve, "fopAcM_delete(id)", error),
        addPre<ActorCreateDisappear>(
            onCreateDisappearPre, kObserve, "fopAcM_createDisappear", error),
        addPre<EbsDamageCheck>(onEbsDamageCheckPre, kObserve, "e_bs damage_check", error),
        addPost<EbsDamageCheck>(onEbsDamageCheckPost, kDefault, "e_bs damage_check", error),
        addPre<EocExecuteFallDead>(onEocFallDeadPre, kObserve, "daE_OC_c::executeFallDead", error),
        addPost<EocExecuteFallDead>(
            onEocFallDeadPost, kDefault, "daE_OC_c::executeFallDead", error),
        addPost<Es1AllFail>(onEs1AllFailPost, kDefault, "e_s1 all_fail", error),
        addPre<Es1Shout>(onEs1ShoutPre, kObserve, "e_s1_shout", error),
        addPost<Es1Shout>(onEs1ShoutPost, kDefault, "e_s1_shout", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
