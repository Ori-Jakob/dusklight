#include "hooks/Hooks.hpp"

#include "autotest/AutoTest.hpp"

#include "d/d_com_inf_game.h"

#include "SSystem/SComponent/c_phase.h"  // d_s_logo.h is not self-contained
#include "d/d_s_logo.h"
#include "m_Do/m_Do_Reset.h"

#include <dolphin/pad.h>

namespace twili::hooks {

DEFINE_HOOK(&dScnLogo_c::nextSceneChange, LogoNextSceneChange);
DEFINE_HOOK(PADRead, PadRead);
DEFINE_HOOK(static_cast<void (*)(const char*, s16, s8, s8, f32, u32, int, s8, s16, int, int)>(
                &dComIfGp_setNextStage),
    SetNextStageTest);

namespace {

HookAction onLogoNextSceneChange(ModContext*, void* args, void*, void*) {
    if (mDoRst::isReset()) {
        return HOOK_CONTINUE;
    }
    auto* logo = mods::arg<dScnLogo_c*>(args, 0);
    return autotest::takeOverBoot(logo) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

void onPadReadPost(ModContext*, void* args, void*, void*) {
    autotest::overridePad(mods::arg<PADStatus*>(args, 0));
}

// At the randomizer's priority: stands in for its entrance shuffle.
HookAction onSetNextStageTest(ModContext*, void* args, void*, void*) {
    autotest::remapStageRequest(mods::arg_ref<const char*>(args, 0), mods::arg_ref<s16>(args, 1),
        mods::arg_ref<s8>(args, 2), mods::arg_ref<s8>(args, 3));
    return HOOK_CONTINUE;
}

}  // namespace

ModResult installAutotest(std::string& error) {
    ModResult result = addPre<LogoNextSceneChange>(
        onLogoNextSceneChange, kDefault, "dScnLogo_c::nextSceneChange", error);
    if (result != MOD_OK) {
        return result;
    }
    result = addPre<SetNextStageTest>(onSetNextStageTest, kDefault, "dComIfGp_setNextStage", error);
    if (result != MOD_OK) {
        return result;
    }
    // Last, so no other mod's pad hook can put real input back.
    return addPost<PadRead>(onPadReadPost, kLate, "PADRead", error);
}

}  // namespace twili::hooks
