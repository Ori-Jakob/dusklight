#include "hooks/Hooks.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "story/Story.hpp"
#include "story/StoryNpc.hpp"
#include "sync/WorldSync.hpp"

#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"

#include <cstring>

namespace twili::hooks {

DEFINE_HOOK(&dEvt_control_c::setParam, EventSetParam);
DEFINE_HOOK(&daObjDrop_c::dropGet, ObjDropGet);
DEFINE_HOOK(&daNpcT_c::evtOrder, NpcTEvtOrder);
DEFINE_HOOK(static_cast<void (*)(const char*, s16, s8, s8, f32, u32, int, s8, s16, int, int)>(
                &dComIfGp_setNextStage),
    SetNextStage);

namespace {

// Before setParam writes the map event's switch: the pull-in announcement goes out first.
HookAction onSetParamPre(ModContext*, void* args, void*, void*) {
    auto* order = mods::arg<dEvt_order_c*>(args, 1);
    if (Session::active() && order != nullptr) {
        story::onEventAccepted(*order);
    }
    return HOOK_CONTINUE;
}

// Which table entry an NPC orders (the originator's), or the entry a join places (the joiner's).
HookAction onNpcTEvtOrderPre(ModContext*, void* args, void*, void*) {
    if (Session::active()) {
        story::npc::beforeEvtOrder(mods::arg<daNpcT_c*>(args, 0));
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

// After every other mod's pre-hook (the randomizer rewrites shuffled entrances in place).
constexpr int32_t kAfterAll = -1000000;

// Our own request keeps its destination; the game's exits stay the other mods' business.
HookAction onSetNextStageLast(ModContext*, void* args, void*, void*) {
    const local::OwnStageRequest* own = local::OwnStageRequest::current();
    if (own == nullptr) {
        return HOOK_CONTINUE;
    }
    auto& stage = mods::arg_ref<const char*>(args, 0);
    auto& point = mods::arg_ref<s16>(args, 1);
    auto& room = mods::arg_ref<s8>(args, 2);
    auto& layer = mods::arg_ref<s8>(args, 3);
    auto& lastMode = mods::arg_ref<u32>(args, 5);
    if (stage == nullptr || std::strncmp(stage, own->stage, 8) != 0 || point != own->point ||
        room != own->room || layer != own->layer)
    {
        TwiliLog.info("[story] another mod remapped our load {}/{}/{}/{} -> {}/{}/{}/{}; restored",
            own->stage, own->room, own->point, own->layer, stage != nullptr ? stage : "-", room,
            point, layer);
        stage = own->stage;
        point = own->point;
        room = own->room;
        layer = own->layer;
    }
    lastMode = 0;
    return HOOK_CONTINUE;
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
    if (result == MOD_OK) {
        result = addPre<SetNextStage>(
            onSetNextStageLast, kAfterAll, "dComIfGp_setNextStage", error);
    }
    if (result == MOD_OK) {
        result = addPre<NpcTEvtOrder>(onNpcTEvtOrderPre, kObserve, "daNpcT_c::evtOrder", error);
    }
    return result;
}

}  // namespace twili::hooks
