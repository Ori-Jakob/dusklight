#include "hooks/Hooks.hpp"

#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include "SSystem/SComponent/c_phase.h"  // d_s_name.h is not self-contained
#include "d/d_com_inf_game.h"
#include "d/d_s_name.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_Reset.h"

namespace twili::hooks {

DEFINE_HOOK(&dSv_info_c::init, SaveInfoInit);
DEFINE_HOOK(&dSv_info_c::card_to_memory, SaveCardToMemory);
DEFINE_HOOK(&dSv_info_c::getSave, SaveGetSave);
DEFINE_HOOK(&dStage_Delete, StageDelete);
DEFINE_HOOK(&dScnName_c::changeGameScene, NameChangeGameScene);

namespace {

bool s_nameStartsGame = false;

HookAction onSaveInfoInitPre(ModContext*, void*, void*, void*) {
    clearSaveLoaded();
    return HOOK_CONTINUE;
}

void onCardToMemoryPost(ModContext*, void*, void* retval, void*) {
    if (retval != nullptr && *static_cast<int*>(retval) == 0) {
        markSaveLoaded();
    }
}

// The stage's save table is live from here (dStage_stagInfoInit); a merge's own call is not.
void onGetSavePost(ModContext*, void* args, void*, void*) {
    if (Session::active() && !sync::RemoteApplyGuard::active()) {
        Session::instance().onStageSaveTableLoaded(mods::arg<int>(args, 1));
    }
}

void onStageDeletePost(ModContext*, void*, void*, void*) {
    if (Session::active()) {
        Session::instance().onStageSaveTableUnloaded();
    }
}

// A new file never goes through card_to_memory.
HookAction onChangeGameScenePre(ModContext*, void*, void*, void*) {
    s_nameStartsGame = !mDoRst::isReset() && !fopOvlpM_IsPeek();
    return HOOK_CONTINUE;
}

void onChangeGameScenePost(ModContext*, void*, void*, void*) {
    if (s_nameStartsGame) {
        markSaveLoaded();
    }
    s_nameStartsGame = false;
}

}  // namespace

ModResult installSave(std::string& error) {
    const ModResult results[] = {
        addPre<SaveInfoInit>(onSaveInfoInitPre, kObserve, "dSv_info_c::init", error),
        addPost<SaveCardToMemory>(onCardToMemoryPost, kDefault, "card_to_memory", error),
        addPost<SaveGetSave>(onGetSavePost, kDefault, "dSv_info_c::getSave", error),
        addPost<StageDelete>(onStageDeletePost, kDefault, "dStage_Delete", error),
        addPre<NameChangeGameScene>(
            onChangeGameScenePre, kObserve, "dScnName_c::changeGameScene", error),
        addPost<NameChangeGameScene>(
            onChangeGameScenePost, kDefault, "dScnName_c::changeGameScene", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
