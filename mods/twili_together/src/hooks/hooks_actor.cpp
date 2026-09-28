#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include "core/GameAccess.hpp"

#include "d/actor/d_a_suspend.h"
#include "f_op/f_op_actor_mng.h"

namespace twili::hooks {

DEFINE_HOOK(static_cast<void (*)(fopAc_ac_c*)>(&daSus_c::check), SuspendCheck);

namespace {

// A dummy is never suspended: one spawned inside a suspend region would stay frozen there.
HookAction onSuspendCheckPre(ModContext*, void* args, void*, void*) {
    perf::count(perf::Target::SuspendCheck);
    auto* actor = mods::arg<fopAc_ac_c*>(args, 0);
    if (!isDummyPlayer(actor)) {
        return HOOK_CONTINUE;
    }
    fopAcM_OffStatus(actor, 0x20000000);
    return HOOK_SKIP_ORIGINAL;
}

}  // namespace

ModResult installActor(std::string& error) {
    return addPre<SuspendCheck>(onSuspendCheckPre, kDefault, "daSus_c::check", error);
}

}  // namespace twili::hooks
