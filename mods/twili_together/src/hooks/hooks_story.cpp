#include "hooks/Hooks.hpp"

#include "core/Session.hpp"
#include "story/Story.hpp"

#include "d/d_event.h"

namespace twili::hooks {

DEFINE_HOOK(&dEvt_control_c::setParam, EventSetParam);

namespace {

// Before setParam writes the map event's switch: the pull-in announcement goes out first.
HookAction onSetParamPre(ModContext*, void* args, void*, void*) {
    auto* order = mods::arg<dEvt_order_c*>(args, 1);
    if (Session::active() && order != nullptr) {
        story::onEventAccepted(*order);
    }
    return HOOK_CONTINUE;
}

}  // namespace

ModResult installStory(std::string& error) {
    return addPre<EventSetParam>(onSetParamPre, kObserve, "dEvt_control_c::setParam", error);
}

}  // namespace twili::hooks
