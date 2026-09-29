#include "hooks/Hooks.hpp"

#include "core/Session.hpp"
#include "story/Story.hpp"
#include "sync/WorldSync.hpp"

#include "d/actor/d_a_obj_drop.h"
#include "d/d_event.h"

namespace twili::hooks {

DEFINE_HOOK(&dEvt_control_c::setParam, EventSetParam);
DEFINE_HOOK(&daObjDrop_c::dropGet, ObjDropGet);

namespace {

// Before setParam writes the map event's switch: the pull-in announcement goes out first.
HookAction onSetParamPre(ModContext*, void* args, void*, void*) {
    auto* order = mods::arg<dEvt_order_c*>(args, 1);
    if (Session::active() && order != nullptr) {
        story::onEventAccepted(*order);
    }
    return HOOK_CONTINUE;
}

// A tear pickup: one LIGHT_DROP instead of its TBOX flag.
HookAction onDropGetPre(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        sync::beforeDropGet(mods::arg<daObjDrop_c*>(args, 0));
    }
    return HOOK_CONTINUE;
}

void onDropGetPost(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        sync::afterDropGet(mods::arg<daObjDrop_c*>(args, 0));
    }
}

}  // namespace

ModResult installStory(std::string& error) {
    ModResult result =
        addPre<EventSetParam>(onSetParamPre, kObserve, "dEvt_control_c::setParam", error);
    if (result == MOD_OK) {
        result = addPre<ObjDropGet>(onDropGetPre, kObserve, "daObjDrop_c::dropGet", error);
    }
    if (result == MOD_OK) {
        result = addPost<ObjDropGet>(onDropGetPost, kDefault, "daObjDrop_c::dropGet", error);
    }
    return result;
}

}  // namespace twili::hooks
