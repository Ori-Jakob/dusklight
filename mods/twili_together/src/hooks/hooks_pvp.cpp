#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include "pvp/Pvp.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_cc_d.h"

namespace twili::hooks {

DEFINE_HOOK(&daAlink_c::checkDamageAction, LinkCheckDamageAction);
DEFINE_HOOK(&dCcD_GObjInf::GetTgHitGObj, GObjGetTgHitGObj);
DEFINE_HOOK(&daAlink_c::setGuardSe, LinkSetGuardSe);
DEFINE_HOOK(&daAlink_c::setDamagePoint, LinkSetDamagePoint);
DEFINE_HOOK(&daAlink_c::checkSpecialNpc, LinkCheckSpecialNpc);

namespace {

// A10: a received hit is planted into mTgCyls[0] for the damage check and removed after it.
HookAction onCheckDamageActionPre(ModContext*, void* args, void*, void*) {
    perf::count(perf::Target::DamageCheck);
    pvp::beginDamageCheck(mods::arg<daAlink_c*>(args, 0));
    return HOOK_CONTINUE;
}

void onCheckDamageActionPost(ModContext*, void* args, void*, void*) {
    pvp::endDamageCheck(mods::arg<daAlink_c*>(args, 0));
}

// The TG branch's first out-of-line call on the hit collider: the branch took our hit.
HookAction onGetTgHitGObjPre(ModContext*, void* args, void*, void*) {
    perf::count(perf::Target::TgHitGObj);
    pvp::onTgBranchEntered(mods::arg<const dCcD_GObjInf*>(args, 0));
    return HOOK_CONTINUE;
}

HookAction onSetGuardSePre(ModContext*, void* args, void*, void*) {
    pvp::onGuardSe(mods::arg<daAlink_c*>(args, 0), mods::arg<const dCcD_GObjInf*>(args, 1));
    return HOOK_CONTINUE;
}

HookAction onSetDamagePointPre(ModContext*, void* args, void*, void*) {
    pvp::onDamagePoint(mods::arg<daAlink_c*>(args, 0));
    return HOOK_CONTINUE;
}

// Via checkEnemyGroup and checkGuardActionChange: battle stance and shield on a lockable player.
void onCheckSpecialNpcPost(ModContext*, void* args, void* retval, void*) {
    if (retval != nullptr && pvp::lockOnTarget(mods::arg<fopAc_ac_c*>(args, 0))) {
        *static_cast<BOOL*>(retval) = TRUE;
    }
}

}  // namespace

ModResult installPvp(std::string& error) {
    const ModResult results[] = {
        addPre<LinkCheckDamageAction>(
            onCheckDamageActionPre, kObserve, "daAlink_c::checkDamageAction", error),
        addPost<LinkCheckDamageAction>(
            onCheckDamageActionPost, kDefault, "daAlink_c::checkDamageAction", error),
        addPre<GObjGetTgHitGObj>(onGetTgHitGObjPre, kObserve, "dCcD_GObjInf::GetTgHitGObj", error),
        addPre<LinkSetGuardSe>(onSetGuardSePre, kObserve, "daAlink_c::setGuardSe", error),
        addPre<LinkSetDamagePoint>(
            onSetDamagePointPre, kObserve, "daAlink_c::setDamagePoint", error),
        addPost<LinkCheckSpecialNpc>(
            onCheckSpecialNpcPost, kDefault, "daAlink_c::checkSpecialNpc", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
