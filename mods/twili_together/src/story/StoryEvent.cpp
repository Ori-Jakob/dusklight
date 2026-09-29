// STORY_EVENT: same-room pull-in; teammates standing safely in the room place the same event.

#include "story/StoryState.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "story/StoryNpc.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "sync/WorldSync.hpp"
#include "teleport/Teleport.hpp"
#include "ui/StoryPrompt.hpp"
#include "ui/Toasts.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_event_manager.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <chrono>
#include <cstring>

namespace twili::story::detail {
namespace {

// A jump lands within this: the join waits for it instead of missing the cutscene.
constexpr uint32_t kAirborneGraceTicks = 60;
// An NPC orders only while no event runs or orders are open: it gets longer.
constexpr uint32_t kNpcOrderTicks = 60;
// An order not accepted by then (outranked, or the order list full) is placed again.
constexpr uint32_t kOrderAcceptTicks = 10;
constexpr int kMaxOrderRetries = 3;
// Watchdog for a copy whose staff never finishes on our side.
constexpr auto kHungAfterOriginEnd = std::chrono::seconds(30);
constexpr auto kHungWithoutEnd = std::chrono::seconds(300);

const char* currentStage() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

bool isEventTag(int16_t profile) {
    return profile == fpcNm_TAG_EVENT_e || profile == fpcNm_TAG_EVT_e ||
           profile == fpcNm_TAG_EVTAREA_e;
}

// Only when our own state kept us out, not while loading, in our own cutscene or elsewhere.
bool missToastWorthy(const char* reason) {
    for (const char* quiet : {"loading", "cutscene", "room", "seen", "different-event", "trigger",
             "ended", "refused", "state", "npc-missing", "npc-event", "npc-not-allowlisted",
             "npc-randomizer", "npc-data"})
    {
        if (std::strcmp(reason, quiet) == 0) {
            return false;
        }
    }
    return true;
}

void missToast(const std::string& name, const std::string& reason) {
    if (missToastWorthy(reason.c_str())) {
        ui::toast("Story",
            fmt::format("You missed {}'s cutscene ({}).", name, teleport::reasonText(reason)),
            ui::kToastStory, 4000);
    }
}

bool npcEvent(const Instance& in) {
    return in.reqKind == ReqKind::Actor && npc::isNpcT(in.requester);
}

const char* requesterKind(const Instance& in) {
    if (npcEvent(in)) {
        return "npc";
    }
    if (in.requester == -1) {
        return "none";
    }
    if (in.requesterIsPlayer) {
        return "player";
    }
    if (isEventTag(in.requester)) {
        return "tag";
    }
    return "actor";
}

// Why the originator's instance cannot be joined, or nullptr.
const char* notJoinable(const Instance& in) {
    if (in.continuation) {
        return "continuation";
    }
    if (in.copyOf != 0) {
        return "copy";
    }
    if (in.orderType != dEvt_type_OTHER_e || in.eventId == -1) {
        return "not-a-demo";
    }
    // An NPC event: the joiner's own NPC orders the same table entry.
    if (npcEvent(in)) {
        if (npc::joinsOff()) {
            return "npc-randomizer";
        }
        if (in.npcIndex <= 0) {
            return "npc-unknown-entry";
        }
        return npc::joinable(in.requester, in.name) ? nullptr : "npc-not-allowlisted";
    }
    if (in.listType != dEvent_manager_c::BASE_STAGE && in.listType != dEvent_manager_c::BASE_DEMO &&
        (in.listType < dEvent_manager_c::BASE_ROOM0 || in.listType > dEvent_manager_c::BASE_ROOM5))
    {
        return "list";
    }
    if (in.mapToolId == 0xFF) {
        return "not-a-map-event";
    }
    if (in.mapType != dStage_MapEvent_dt_TYPE_ZEV && in.mapType != dStage_MapEvent_dt_TYPE_STB) {
        return "camera";
    }
    // NPC requesters get INDEMO and keep private event state.
    if (in.requester != -1 && !in.requesterIsPlayer && !isEventTag(in.requester)) {
        return "actor-requester";
    }
    // An arrival demo only when it is story: one-shot (a switch) or relocating (an exit).
    if (in.arrivalDemo && in.switchNo == 0xFF && in.exit == 0xFF) {
        return "repeatable-arrival";
    }
    return nullptr;
}

// The map event's table and name as searchMapEventData finds it here.
dStage_MapEvent_dt_c* localMapEvent(
    uint8_t mapToolId, int roomNo, std::string& table, std::string& name) {
    table = "none";
    name.clear();
    dStage_MapEvent_dt_c* found = nullptr;
    if (dStage_roomDt_c* room = dComIfGp_roomControl_getStatusRoomDt(roomNo)) {
        if (dStage_MapEventInfo_c* info = room->getMapEventInfo()) {
            for (int i = 0; i < info->num && found == nullptr; i++) {
                if (info->m_entries[i].field_0x4 == mapToolId) {
                    found = &info->m_entries[i];
                    table = "room";
                }
            }
        }
    }
    if (found == nullptr) {
        if (dStage_MapEventInfo_c* info = dComIfGp_getStage()->getMapEventInfo()) {
            for (int i = 0; i < info->num && found == nullptr; i++) {
                if (info->m_entries[i].field_0x4 == mapToolId) {
                    found = &info->m_entries[i];
                    table = "stage";
                }
            }
        }
    }
    if (found != nullptr &&
        (found->type == dStage_MapEvent_dt_TYPE_ZEV || found->type == dStage_MapEvent_dt_TYPE_STB))
    {
        name.assign(found->data.event_name, strnlen(found->data.event_name, 13));
    }
    return found;
}

struct TagSearch {
    uint8_t eventNo;
    int roomNo;
};

// A daTag_Event_c for `eventNo` in `roomNo` whose trigger would still fire (area skipped).
void* findLiveEventTag(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const auto* want = static_cast<const TagSearch*>(data);
    if (fopAcM_GetName(actor) != fpcNm_TAG_EVENT_e || fopAcM_GetRoomNo(actor) != want->roomNo) {
        return nullptr;
    }
    auto* tag = static_cast<daTag_Event_c*>(actor);
    if (tag->getEventNo() != want->eventNo) {
        return nullptr;
    }
    const int swbit = tag->getSwbit();
    if (swbit != 0xFF && dComIfGs_isSwitch(swbit, want->roomNo)) {
        return nullptr;
    }
    return tag->arrivalTerms() ? actor : nullptr;
}

void orderJoin() {
    JoinInfo& j = state().join;
    if (j.npc) {
        daNpcT_c* npc = static_cast<daNpcT_c*>(fopAcM_SearchByID(j.npcId));
        npc::placeOrder(npc, j.npcIndex);
        j.state = JoinState::Ordered;
        j.startTick = nowTick();
        TwiliLog.info("[story] join: our NPC {} orders entry {} '{}'", j.npcId, j.npcIndex,
            j.eventName);
        return;
    }
    const int stay = dComIfGp_roomControl_getStayNo();
    const s16 evId = dComIfGp_getEventManager().getEventIdx(nullptr, j.mapToolId, -1);
    // Never 0xE00: change() takes any 0xE00 order whose requester is mChangeActor (NULL here).
    const u16 flag = j.autoNext ? 0x101 : 0x001;
    s32 placed = 0;
    if (j.bypassSwitch && j.switchNo != 0xFF && dComIfGs_isSwitch(j.switchNo, stay)) {
        // The originator's SET_FLAG landed and order() would refuse: off, order, on again, unsent.
        sync::RemoteApplyGuard guard;
        dComIfGs_offSwitch(j.switchNo, stay);
        placed = fopAcM_orderMapToolEvent(nullptr, j.mapToolId, evId, 0xFFFF, flag, 5);
        dComIfGs_onSwitch(j.switchNo, stay);
    } else {
        placed = fopAcM_orderMapToolEvent(nullptr, j.mapToolId, evId, 0xFFFF, flag, 5);
    }
    j.state = JoinState::Ordered;
    j.startTick = nowTick();
    TwiliLog.info("[story] join ordered map {} ev {} flag 0x{:X}{} -> {}", j.mapToolId, evId, flag,
        j.bypassSwitch ? " (switch bypass)" : "", placed);
}

}  // namespace

// The local player may be pulled into a cutscene now, or the reason code it may not.
const char* pullInBlocker() {
    State& st = state();
    if (!st.forcedBlocker.empty()) {
        return st.forcedBlocker.c_str();
    }
    if (const char* why = local::localTeleportBlocker()) {
        return why;
    }
    if (loadActive() || !teleport::idle()) {
        return "pending";
    }
    if (ui::anyDocumentVisible()) {
        return "menu";
    }
    daAlink_c* link = local::liveLink();
    if (link->mLinkAcch.ChkWaterIn() || link->checkModeFlg(daAlink_c::MODE_SWIMMING)) {
        return "swimming";
    }
    if (!local::isSettledOnGround(link)) {
        return "airborne";
    }
    return nullptr;
}

void onInstanceAccepted(const Instance& in) {
    State& st = state();
    JoinInfo& j = st.join;
    // Our copy of a teammate's cutscene was accepted (joinClaims marked it).
    if (in.copyOf != 0 && !in.continuation && j.state == JoinState::Ordered &&
        in.copyOf == j.originInstance)
    {
        j.state = JoinState::Running;
        j.localInstance = in.id;
        j.runningSince = Clock::now();
        TwiliLog.info("[story] join running: {}'s '{}' (their #{}, ours #{})", j.originName,
            j.eventName, j.originInstance, in.id);
        sendPacket({{"type", "STORY_EVENT"}, {"ph", "joined"}, {"id", j.originInstance}});
        ui::toast(
            "Story", fmt::format("Joining {}'s cutscene.", j.originName), ui::kToastStory, 3000);
        return;
    }
    const Session& session = Session::instance();
    if (!session.isConnected() || !sync::enabled() || !session.roomState().cutsceneSync ||
        (in.mapToolId == 0xFF && !npcEvent(in)))
    {
        return;
    }
    if (const char* why = notJoinable(in)) {
        if (!in.continuation && in.copyOf == 0) {
            TwiliLog.info("[story] event #{} '{}' not joinable ({})", in.id, in.name, why);
        }
        return;
    }
    nlohmann::json packet = {
        {"type", "STORY_EVENT"},
        {"ph", "start"},
        {"id", in.id},
        {"stage", std::string(in.stage)},
        {"room", in.room},
        {"layer", in.layer},
        {"m", in.mapToolId},
        {"name", in.name},
        {"lt", in.listType},
        {"mt", in.mapType},
        {"tbl", in.table == EventTable::Room ? "room" : "stage"},
        {"sw", in.switchNo},
        {"next", in.next},
        {"req", requesterKind(in)},
        {"flag", in.flag},
        {"arrival", in.arrivalDemo},
        {"wolf", in.wolf},
        {"quiet", true},
    };
    if (in.tagEventNo != 0xFF) {
        packet["tag"] = {{"no", in.tagEventNo}, {"swbit", in.tagSwbit}};
    }
    if (npcEvent(in)) {
        const SpawnKey& k = in.reqKey;
        packet["npc"] = {{"prof", k.procName}, {"room", k.roomNo}, {"params", k.params},
            {"set", k.setId}, {"home", {k.home[0], k.home[1], k.home[2]}}, {"k", in.npcIndex},
            {"sd", fmt::format("{:016x}", npc::storyDigest())}};
    }
    sendPacket(std::move(packet));
    st.announcedInstance = in.id;
    st.joinedBy.clear();
    TwiliLog.info("[story] event #{} '{}' announced (map {}, {} requester)", in.id, in.name,
        in.mapToolId, requesterKind(in));
}

uint32_t joinClaims(const dEvt_order_c& order) {
    const JoinInfo& j = state().join;
    if (j.state != JoinState::Ordered) {
        return 0;
    }
    if (j.npc) {
        // Our NPC ordered the entry through its own path: it is the requester.
        return order.mpRequestActor != nullptr && fopAcM_GetID(order.mpRequestActor) == j.npcId ?
                   j.originInstance :
                   0;
    }
    if (order.mpRequestActor != nullptr || order.mMapToolId != j.mapToolId) {
        return 0;
    }
    return j.originInstance;
}

// J3n, J4n, J9: our copy of the NPC, the same table entry, the same story state.
const char* checkNpcJoin(const nlohmann::json& packet, const std::string& eventName, JoinInfo& j) {
    const auto it = packet.find("npc");
    if (it == packet.end() || !it->is_object()) {
        return "npc-data";
    }
    if (npc::joinsOff()) {
        return "npc-randomizer";
    }
    SpawnKey key;
    key.procName = static_cast<int16_t>(it->value("prof", -1));
    key.roomNo = static_cast<int8_t>(it->value("room", -1));
    key.params = it->value("params", 0u);
    key.setId = static_cast<uint16_t>(it->value("set", 0));
    const auto home = it->find("home");
    for (int i = 0; home != it->end() && home->is_array() && home->size() == 3 && i < 3; i++) {
        key.home[i] = (*home)[i].is_number() ? (*home)[i].get<float>() : 0.0f;
    }
    const int index = it->value("k", 0);
    if (!npc::isNpcT(key.procName) || !npc::joinable(key.procName, eventName) || index <= 0) {
        return "npc-not-allowlisted";
    }
    daNpcT_c* npc = npc::findNpc(key);
    if (npc == nullptr) {
        return "npc-missing";
    }
    if (npc::eventName(npc, index) != eventName) {
        return "npc-event";
    }
    if (it->value("sd", std::string{}) != fmt::format("{:016x}", npc::storyDigest())) {
        return "state";
    }
    j.npc = true;
    j.npcIndex = static_cast<int16_t>(index);
    j.npcId = fopAcM_GetID(npc);
    return nullptr;
}

void handleStoryEvent(const nlohmann::json& packet) {
    if (!acceptsPacket(packet, true)) {
        return;
    }
    State& st = state();
    const std::string ph = packet.value("ph", std::string{});
    const uint32_t from = packet.value("clientId", 0u);
    const uint32_t id = packet.value("id", 0u);
    JoinInfo& j = st.join;
    if (ph == "joined") {
        if (id != 0 && id == st.announcedInstance) {
            st.joinedBy.insert(from);
            TwiliLog.info("[story] event #{} joined by {}", id, teleport::clientName(from));
        }
        return;
    }
    if (ph == "end") {
        if (j.originClientId == from && j.originInstance == id) {
            j.originEnded = true;
            j.originEndedAt = Clock::now();
            if (j.state == JoinState::Waiting) {
                j.state = JoinState::Missed;
                j.reason = "ended";
            }
        }
        return;
    }
    if (ph != "start") {
        return;
    }

    const std::string name = teleport::clientName(from);
    const uint8_t m = static_cast<uint8_t>(packet.value("m", 0xFF) & 0xFF);
    const std::string eventName = packet.value("name", std::string{}).substr(0, 16);
    const int room = packet.value("room", -1);
    const int stay = dComIfGp_roomControl_getStayNo();
    const auto miss = [&](const char* reason) {
        j = JoinInfo{};
        j.state = JoinState::Missed;
        j.reason = reason;
        j.originClientId = from;
        j.originInstance = id;
        j.originName = name;
        j.eventName = eventName;
        TwiliLog.info("[story] join missed {}'s '{}' ({})", name, eventName, reason);
        missToast(name, reason);
    };

    // Same stage, layer and room.
    if (std::strncmp(currentStage(), packet.value("stage", std::string{}).c_str(), 8) != 0 ||
        dComIfG_play_c::getLayerNo(0) != packet.value("layer", -1) || stay != room)
    {
        return miss("room");
    }
    const std::string req = packet.value("req", std::string{});
    if (req == "npc") {
        JoinInfo npcJoin;
        if (dComIfGp_event_runCheck()) {
            return miss("cutscene");
        }
        if (const char* why = checkNpcJoin(packet, eventName, npcJoin)) {
            return miss(why);
        }
        if (packet.value("wolf", false) != (daPy_py_c::checkNowWolf() != FALSE)) {
            return miss("form");
        }
        if (const char* why = pullInBlocker()) {
            return miss(why);
        }
        j = npcJoin;
        j.originClientId = from;
        j.originInstance = id;
        j.originName = name;
        j.eventName = eventName;
        orderJoin();
        return;
    }
    // The same map event exists here, from the same table, with the same name.
    std::string table, localName;
    dStage_MapEvent_dt_c* dt = localMapEvent(m, stay, table, localName);
    if (dt == nullptr || table != packet.value("tbl", std::string{}) ||
        dt->type != packet.value("mt", -1) || localName != eventName)
    {
        return miss("different-event");
    }
    // Already in an event: both triggered it, or our auto-next plays the chain successor.
    if (dComIfGp_event_runCheck()) {
        const Instance* open = tracker().openInstance();
        if (open != nullptr && open->running() && (open->mapToolId == m || open->next == m)) {
            j = JoinInfo{};
            j.state = JoinState::Shared;
            j.originClientId = from;
            j.originInstance = id;
            j.originName = name;
            j.eventName = eventName;
            TwiliLog.info("[story] join shared: {}'s '{}' runs here too", name, eventName);
            return;
        }
        return miss("cutscene");
    }
    // Not seen yet.
    const uint8_t sw = static_cast<uint8_t>(packet.value("sw", 0xFF) & 0xFF);
    if (sw != 0xFF && dComIfGs_isSwitch(sw, stay)) {
        return miss("seen");
    }
    // A tag's own trigger terms hold here too.
    if (req == "tag") {
        const auto tag = packet.find("tag");
        if (tag != packet.end() && tag->is_object()) {
            TagSearch search{static_cast<uint8_t>(tag->value("no", 0xFF) & 0xFF), stay};
            if (fopAcM_Search(&findLiveEventTag, &search) == nullptr) {
                return miss("trigger");
            }
        }
    }
    // Same form.
    if (packet.value("wolf", false) != (daPy_py_c::checkNowWolf() != FALSE)) {
        return miss("form");
    }

    j = JoinInfo{};
    j.originClientId = from;
    j.originInstance = id;
    j.originName = name;
    j.eventName = eventName;
    j.mapToolId = m;
    j.switchNo = sw;
    // Ordered with AutoNext, our copy chains its successor itself (the tag does not run).
    j.autoNext =
        (packet.value("flag", 0) & 0x300) != 0 || req == "tag" || packet.value("arrival", false);
    const char* why = pullInBlocker();
    if (why != nullptr && std::strcmp(why, "airborne") == 0) {
        j.state = JoinState::Waiting;
        j.startTick = nowTick();
        j.reason = why;
        TwiliLog.info("[story] join waits for us to land ({}'s '{}')", name, eventName);
        return;
    }
    if (why != nullptr) {
        return miss(why);
    }
    orderJoin();
}

void tickJoin() {
    State& st = state();
    // Our announced instance can no longer be continued: close it for the joiners.
    if (st.announcedInstance != 0) {
        const Instance* open = tracker().openInstance();
        if (open == nullptr || open->id != st.announcedInstance) {
            if (Session::instance().isConnected()) {
                sendPacket({{"type", "STORY_EVENT"}, {"ph", "end"}, {"id", st.announcedInstance},
                    {"quiet", true}});
            }
            st.announcedInstance = 0;
            st.joinedBy.clear();
        }
    }

    JoinInfo& j = st.join;
    const uint32_t tick = nowTick();
    const auto now = Clock::now();
    switch (j.state) {
    case JoinState::Waiting: {
        const char* why = pullInBlocker();
        if (why == nullptr) {
            j.bypassSwitch = true;
            orderJoin();
        } else if (std::strcmp(why, "airborne") != 0 || tick - j.startTick > kAirborneGraceTicks) {
            j.state = JoinState::Missed;
            j.reason = why;
            TwiliLog.info("[story] join missed {}'s '{}' ({})", j.originName, j.eventName, why);
            missToast(j.originName, j.reason);
        }
        return;
    }
    case JoinState::Ordered:
        if (j.npc && tick - j.startTick <= kNpcOrderTicks) {
            return;
        }
        if (tick - j.startTick > kOrderAcceptTicks) {
            if (j.retries >= kMaxOrderRetries || dComIfGp_event_runCheck()) {
                j.state = JoinState::Missed;
                j.reason = "refused";
                npc::placeOrder(nullptr, 0);
                TwiliLog.info("[story] join of '{}' was never accepted", j.eventName);
                return;
            }
            j.retries++;
            j.bypassSwitch = true;
            orderJoin();
        }
        return;
    case JoinState::Running: {
        const Instance* open = tracker().openInstance();
        if (open == nullptr || open->id != j.localInstance) {
            j.state = JoinState::Ended;
            TwiliLog.info("[story] join of {}'s '{}' ended", j.originName, j.eventName);
            return;
        }
        const bool hung = (j.originEnded && now - j.originEndedAt > kHungAfterOriginEnd) ||
                          now - j.runningSince > kHungWithoutEnd;
        if (hung && open->running()) {
            // The sequencer then follows the event's exit like a skip.
            TwiliLog.warn("[story] join of '{}' hung; ending it", j.eventName);
            dComIfGp_event_reset();
            j.state = JoinState::Ended;
            j.reason = "hung";
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace twili::story::detail

namespace twili::story {

bool sharedEventWith(uint32_t clientId) {
    const detail::State& st = detail::state();
    const detail::JoinInfo& j = st.join;
    if (j.state == JoinState::Running && j.originClientId == clientId && !j.originEnded) {
        return true;
    }
    return st.announcedInstance != 0 && st.joinedBy.count(clientId) != 0;
}

}  // namespace twili::story
