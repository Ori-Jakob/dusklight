// Event instances from the setParam hook, and our own story moves (departure, arrival, settle).

#include "story/StoryTracker.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "story/StoryNpc.hpp"
#include "story/StoryState.hpp"
#include "sync/WorldSync.hpp"
#include "teleport/Teleport.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_tag_event.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_event_manager.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace twili::story {
namespace {

// An instance stays open this long after it ends, so a tag's actionNext still continues it.
constexpr uint32_t kContinueTicks = 30;
// An actor may set the next stage just as its event ends.
constexpr uint32_t kDepartAfterEndTicks = 3;
constexpr size_t kMaxInstances = 16;
// A move settles once no event ran for this long after arrival.
constexpr uint32_t kSettleQuietTicks = 45;
constexpr auto kSettleMax = std::chrono::seconds(180);
constexpr auto kArrivalTimeout = std::chrono::seconds(60);
constexpr int kMaxHops = 4;
constexpr size_t kBitLogSize = 64;

std::string mapEventName(const dStage_MapEvent_dt_c* dt) {
    if (dt == nullptr) {
        return {};
    }
    if (dt->type == dStage_MapEvent_dt_TYPE_ZEV || dt->type == dStage_MapEvent_dt_TYPE_STB) {
        return std::string(dt->data.event_name, strnlen(dt->data.event_name, 13));
    }
    return fmt::format("MapToolCamera{}", dt->field_0x4);
}

// searchMapEventData, also saying which table matched (the room's comes first).
dStage_MapEvent_dt_c* findMapEvent(uint8_t mapToolId, int roomNo, EventTable& table) {
    table = EventTable::None;
    if (mapToolId == 0xFF) {
        return nullptr;
    }
    if (dStage_roomDt_c* room = dComIfGp_roomControl_getStatusRoomDt(roomNo)) {
        if (dStage_MapEventInfo_c* info = room->getMapEventInfo()) {
            for (int i = 0; i < info->num; i++) {
                if (info->m_entries[i].field_0x4 == mapToolId) {
                    table = EventTable::Room;
                    return &info->m_entries[i];
                }
            }
        }
    }
    if (dStage_MapEventInfo_c* info = dComIfGp_getStage()->getMapEventInfo()) {
        for (int i = 0; i < info->num; i++) {
            if (info->m_entries[i].field_0x4 == mapToolId) {
                table = EventTable::Stage;
                return &info->m_entries[i];
            }
        }
    }
    return nullptr;
}

std::string eventDataName(int16_t eventId) {
    if (eventId == -1) {
        return {};
    }
    dEvDtEvent_c* data = dComIfGp_getEventManager().getEventData(eventId);
    return data != nullptr ? std::string(data->getName()) : std::string{};
}

const char* tableName(EventTable t) {
    switch (t) {
    case EventTable::Room:
        return "room";
    case EventTable::Stage:
        return "stage";
    default:
        return "none";
    }
}

// What the saved flags pick for a stage: temp bits (King Bulblin's field layers) left out.
int savedFlagLayer(const char* stage, int room) {
    dSv_event_c& tmp = dComIfGs_getSaveInfo()->getTmp();
    uint8_t saved[sizeof(tmp.mEvent)];
    std::memcpy(saved, tmp.mEvent, sizeof(saved));
    std::memset(tmp.mEvent, 0, sizeof(tmp.mEvent));
    const int layer = dComIfG_play_c::getLayerNo_common(stage, room, -1);
    std::memcpy(tmp.mEvent, saved, sizeof(saved));
    return layer;
}

std::string entranceText(const Entrance& e) {
    return fmt::format(
        "{} room {} point {} layer {}/{}", e.stage, e.room, e.point, e.layerArg, e.layer);
}

std::string newMoveId() {
    static std::mt19937_64 rng{std::random_device{}()};
    return fmt::format("{:016x}", rng());
}

const char* currentStageName() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

int saveTblNo() {
    return Session::instance().currentSaveTblNo();
}

bool isEventTag(int16_t profile) {
    return profile == fpcNm_TAG_EVENT_e || profile == fpcNm_TAG_EVT_e ||
           profile == fpcNm_TAG_EVTAREA_e;
}

ReqKind reqKindOf(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return ReqKind::None;
    }
    if (actor == dComIfGp_getPlayer(0)) {
        return ReqKind::Player;
    }
    return isEventTag(fopAcM_GetName(const_cast<fopAc_ac_c*>(actor))) ? ReqKind::Tag :
                                                                       ReqKind::Actor;
}

}  // namespace

uint32_t nowTick() {
    return g_Counter.mCounter0;
}

uint8_t transformLevels() {
    return dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusB().mTransformLevelFlag;
}

uint8_t darkClearLevels() {
    return dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusB().mDarkClearLevelFlag;
}

Tracker& tracker() {
    static Tracker s_tracker;
    return s_tracker;
}

void Tracker::clear() {
    mInstances.clear();
    mSawEventSinceLoad = false;
    mPrevPending = false;
    mLinkWasLive = false;
    mEventWasRunning = false;
    mMove.reset();
    mLastOwnStoryArrival.reset();
}

void Tracker::onStageSaveTableLoaded() {
    mSawEventSinceLoad = false;
    mInstances.clear();
    mBitsAtStageLoad = mLocalEventBitsSet;
}

void Tracker::noteLocalEventBit(uint16_t no) {
    mLocalEventBitsSet++;
    BitNote note;
    note.index = mLocalEventBitsSet;
    note.no = no;
    note.story = dComIfGp_event_runCheck() != FALSE || mMove.has_value();
    mBitLog.push_back(note);
    while (mBitLog.size() > kBitLogSize) {
        mBitLog.pop_front();
    }
}

const Instance* Tracker::openInstance() const {
    if (mInstances.empty()) {
        return nullptr;
    }
    const Instance& in = mInstances.back();
    if (in.running() || nowTick() - in.endedTick <= kContinueTicks) {
        return &in;
    }
    return nullptr;
}

uint32_t Tracker::ticksSinceLoad() const {
    return mLinkWasLive ? nowTick() - mStageLiveTick : 0;
}

uint32_t Tracker::quietTicks() const {
    if (!mLinkWasLive || mEventWasRunning) {
        return 0;
    }
    const uint32_t since = mLastEventEndTick > mStageLiveTick ? mLastEventEndTick : mStageLiveTick;
    return nowTick() - since;
}

void Tracker::onEventAccepted(const dEvt_order_c& order) {
    if (!isSaveLoaded()) {
        return;
    }
    const int stay = dComIfGp_roomControl_getStayNo();
    Instance in;
    in.seq = mNextSeq++;
    in.eventId = order.mEventId;
    in.listType =
        order.mEventId == -1 ? 0 : static_cast<uint8_t>(static_cast<uint16_t>(order.mEventId) >> 8);
    in.mapToolId = order.mMapToolId;
    in.orderType = order.mEventType;
    in.flag = order.mFlag;
    in.requester = order.mpRequestActor != nullptr ? fopAcM_GetName(order.mpRequestActor) : -1;
    in.requesterIsPlayer =
        order.mpRequestActor != nullptr && order.mpRequestActor == dComIfGp_getPlayer(0);
    in.reqKind = reqKindOf(order.mpRequestActor);
    if (in.reqKind == ReqKind::Actor) {
        in.reqKey = spawnKeyOf(order.mpRequestActor);
        if (npc::isNpcT(in.requester)) {
            in.npcIndex = static_cast<int16_t>(npc::orderedIndex(order.mpRequestActor));
        }
    }
    if (in.requester == fpcNm_TAG_EVENT_e) {
        auto* tag = static_cast<daTag_Event_c*>(order.mpRequestActor);
        in.tagEventNo = tag->getEventNo();
        in.tagSwbit = tag->getSwbit();
    }
    if (dStage_MapEvent_dt_c* dt = findMapEvent(order.mMapToolId, stay, in.table)) {
        in.mapType = dt->type;
        in.switchNo = dt->switch_no;
        in.next = dt->field_0x5;
        in.exit = dt->field_0x7;
        in.skipExit = dt->field_0x9;
        in.name = mapEventName(dt);
    }
    if (in.name.empty()) {
        in.name = eventDataName(order.mEventId);
    }
    std::strncpy(in.stage, currentStageName(), sizeof(in.stage) - 1);
    in.room = static_cast<int8_t>(stay);
    in.layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    daAlink_c* link = daAlink_getAlinkActorClass();
    in.wolf = link != nullptr && link->checkWolf();
    // The stage's arrival start demo: Link orders it with no requester before any other event.
    in.arrivalDemo = !mSawEventSinceLoad && order.mpRequestActor == nullptr && link != nullptr &&
                     order.mMapToolId != 0xFF && order.mMapToolId == link->mStartEventID;
    mSawEventSinceLoad = true;
    in.bitsAtAccept = mLocalEventBitsSet;
    in.acceptedTick = nowTick();

    // Skips, auto-next chains, actor event changes and tag successors continue their event.
    const Instance* open = openInstance();
    if (open != nullptr &&
        ((order.mFlag & 0x600) != 0 || (open->next != 0xFF && order.mMapToolId == open->next)))
    {
        in.id = open->id;
        in.continuation = true;
        in.arrivalDemo = open->arrivalDemo;
        in.copyOf = open->copyOf;
        in.bitsAtAccept = open->bitsAtAccept;
    } else {
        in.id = mNextInstanceId++;
    }
    if (!in.continuation) {
        in.copyOf = detail::joinClaims(order);
    }

    TwiliLog.info("[story] accept #{} seq {} ev {} (list {}) '{}' map {} type {} tbl {} sw {} "
                  "next {} exit {}/{} req {} ({}){} type {} flag 0x{:X} {} room {} layer {} {}{}{}{}",
        in.id, in.seq, in.eventId, in.listType, in.name, in.mapToolId, in.mapType,
        tableName(in.table), in.switchNo, in.next, in.exit, in.skipExit, in.requester,
        reqKindName(in.reqKind),
        in.reqKey.valid() ?
            fmt::format(" [{}{}]", in.reqKey.text(),
                in.npcIndex >= 0 ? fmt::format(" npc entry {}", in.npcIndex) : std::string{}) :
            std::string{},
        in.orderType,
        in.flag, in.stage, in.room, in.layer, in.wolf ? "wolf" : "human",
        in.arrivalDemo ? " arrival" : "", in.continuation ? " continuation" : "",
        in.copyOf != 0 ? fmt::format(" copy of {}", in.copyOf) : std::string{});

    mInstances.push_back(in);
    while (mInstances.size() > kMaxInstances) {
        mInstances.pop_front();
    }
    detail::onInstanceAccepted(mInstances.back());
}

bool Tracker::captureDeparture(PendingMove& out) {
    dEvt_control_c* evt = dComIfGp_getEvent();
    const bool running = dComIfGp_event_runCheck() != FALSE;
    const Instance* last = lastInstance();
    const uint32_t tick = nowTick();
    const bool justEnded =
        last != nullptr && !last->running() && tick - last->endedTick <= kDepartAfterEndTicks;

    MoveRecord& m = out.move;
    m.from.setStage(currentStageName());
    m.from.room = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
    m.from.layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    m.to.setStage(dComIfGp_getNextStageName());
    m.to.room = static_cast<int8_t>(dComIfGp_getNextStageRoomNo());
    const int point = dComIfGp_getNextStagePoint();
    m.to.point = static_cast<int16_t>(point);
    m.to.layerArg = static_cast<int8_t>(dComIfGp_getNextStageLayer());

    if (!running && !justEnded) {
        return false;  // an exit Link walked into, a warp, a void-out
    }
    // endProc waits for the load, so the event's fields stay readable while it is pending.
    m.event.eventId = running ? evt->mEventId : last->eventId;
    m.event.mapToolId = running ? evt->mMapToolId : last->mapToolId;
    m.event.mode = running ? evt->mMode : last->mode;
    const uint16_t eventFlag = running ? evt->mEventFlag : last->eventFlag;
    m.event.listType = m.event.eventId == -1 ?
                           0 :
                           static_cast<uint8_t>(static_cast<uint16_t>(m.event.eventId) >> 8);
    m.bossDefeated = bossStageDungeon(m.from.stage) != nullptr && dComIfGs_isStageBossEnemy();
    if (last != nullptr) {
        m.event.name = last->name;
        m.event.mapType = last->mapType;
        m.event.switchNo = last->switchNo;
        m.event.requester = last->requester;
        m.event.reqKind = last->reqKind;
        m.event.arrivalDemo = last->arrivalDemo;
        out.bitsAtAccept = last->bitsAtAccept;
        out.copyOf = last->copyOf;
    }
    out.bitsAtVisit = mBitsAtStageLoad;
    if (running && (last == nullptr || last->eventId != evt->mEventId)) {
        // Programmatic or not seen by the hook: take what the live event says.
        m.event.name = eventDataName(evt->mEventId);
        EventTable t;
        if (dStage_MapEvent_dt_c* dt = findMapEvent(evt->mMapToolId, m.from.room, t)) {
            m.event.mapType = dt->type;
            m.event.switchNo = dt->switch_no;
            m.event.name = mapEventName(dt);
        }
        fopAc_ac_c* req = evt->getPt1();
        m.event.requester = req != nullptr ? fopAcM_GetName(req) : -1;
        m.event.reqKind = reqKindOf(req);
        out.bitsAtAccept = mLocalEventBitsSet;
    }
    collectBits(out);
    m.curated = matchCuratedMove(m);

    const char* why = nullptr;
    std::string notStory;
    if (!teleport::idle() || loadActive()) {
        why = "twili-load";  // a follow never produces another move
    } else if (isNotStoryRequester(m.event.requester)) {
        notStory = fmt::format("not-story: profile {}", m.event.requester);
        why = notStory.c_str();
    } else if (m.curated < 0) {
        // Potential events (kytag04, bosses) and common ones (BOSS_WARPIN) have actor requesters.
        const bool actor = m.event.reqKind == ReqKind::Actor;
        if (m.event.mode != dEvt_mode_DEMO_e || (eventFlag & 0x44) != 0) {
            why = "not-a-cutscene";  // talk, door or chest
        } else if (!actor && (m.event.eventId == -1 ||
                                 m.event.listType == dEvent_manager_c::BASE_KEEP))
        {
            why = "default-event";  // warps, returns, DEFAULT_START
        } else if (point < 0 || dComIfGs_getLife() <= 0) {
            why = "restart";  // void-out, game over, world change
        } else if (m.to.sameStageRoom(m.from.stage, m.from.room) && m.to.layerArg == -1 &&
                   !(mMove && mMove->arrived))
        {
            // A respawn, unless it continues our move (the cell's wake-up exits in-room).
            why = "in-place";
        }
    }
    TwiliLog.info("[story] depart {} room {} layer {} -> {} ev {} (list {}) '{}' map {} sw {} "
                  "req {} ({}) mode {}{}{} {}",
        m.from.stage, m.from.room, m.from.layer, entranceText(m.to), m.event.eventId,
        m.event.listType, m.event.name, m.event.mapToolId, m.event.switchNo, m.event.requester,
        reqKindName(m.event.reqKind), m.event.mode, m.event.arrivalDemo ? " arrival" : "",
        m.curated >= 0 ? fmt::format(" curated {}", kStoryMoves[m.curated].id) : std::string{},
        why != nullptr ? fmt::format("ignored ({})", why) : std::string("tracked"));
    if (why != nullptr) {
        return false;
    }
    // A follow's destination may relocate once more: recorded, never broadcast.
    if (detail::followChainOpen()) {
        out.copyOf = kFollowChainCopy;
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    m.wolfBefore = link != nullptr && link->checkWolf();
    m.tlvBefore = transformLevels();
    m.dclBefore = darkClearLevels();
    return true;
}

void Tracker::recordArrival() {
    PendingMove& p = *mMove;
    MoveRecord& m = p.move;
    const char* stage = currentStageName();
    if (std::strncmp(stage, m.to.stage, sizeof(m.to.stage)) != 0) {
        TwiliLog.info(
            "[story] move {} -> {} diverted (arrived in {})", m.from.stage, m.to.stage, stage);
        mMove.reset();
        return;
    }
    m.to.point = dComIfGp_getStartStagePoint();
    m.to.room = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
    m.to.layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    daAlink_c* link = local::liveLink();
    m.wolfAfter = link != nullptr && link->checkWolf();
    m.tlvAfter = transformLevels();
    m.dclAfter = darkClearLevels();
    m.arrivalEvent = link != nullptr ? static_cast<uint8_t>(link->mStartEventID) : 0xFF;
    EventTable t;
    const dStage_MapEvent_dt_c* arrivalDt = findMapEvent(m.arrivalEvent, m.to.room, t);
    m.arrivalName = mapEventName(arrivalDt);
    m.curated = matchCuratedMove(m);
    if (m.hopList.size() < kMaxHopRecords) {
        Hop hop;
        hop.at = m.to;
        hop.arrivalMap = m.arrivalEvent;
        hop.arrivalSwitch = arrivalDt != nullptr ? arrivalDt->switch_no : 0xFF;
        hop.arrivalName = m.arrivalName;
        m.hopList.push_back(std::move(hop));
    }
    p.arrived = true;
    p.arrivedAt = Clock::now();
    p.lastTick = nowTick();
    p.quiet = 0;
    TwiliLog.info("[story] arrive {} ({}) tlv 0x{:02X}->0x{:02X} dcl 0x{:02X}->0x{:02X} arrival "
                  "event {} '{}' sw {}",
        entranceText(m.to), m.wolfAfter ? "wolf" : "human", m.tlvBefore, m.tlvAfter, m.dclBefore,
        m.dclAfter, m.arrivalEvent, m.arrivalName,
        arrivalDt != nullptr ? static_cast<int>(arrivalDt->switch_no) : 0xFF);
}

uint32_t Tracker::collectBits(PendingMove& p) const {
    MoveRecord& m = p.move;
    m.bits.clear();
    uint32_t sinceVisit = 0;
    for (const BitNote& note : mBitLog) {
        if (note.index <= p.bitsAtVisit || (note.index <= p.bitsAtAccept && !note.story)) {
            continue;
        }
        sinceVisit++;
        if (m.bits.size() < kMaxMoveBits &&
            std::find(m.bits.begin(), m.bits.end(), note.no) == m.bits.end())
        {
            m.bits.push_back(note.no);
        }
    }
    return sinceVisit;
}

void Tracker::settle() {
    PendingMove p = std::move(*mMove);
    mMove.reset();
    MoveRecord& m = p.move;
    const uint32_t sinceAccept = mLocalEventBitsSet - p.bitsAtAccept;
    // Bits of the departure stage's earlier story events count too (BOSSCLEAR before WARPHOLE).
    const uint32_t sinceVisit = collectBits(p);
    m.curated = matchCuratedMove(m);
    // A cutscene or battle layer: what we loaded is not what our saved flags pick there.
    m.transientHint = m.to.layer >= 0 && m.to.layer != savedFlagLayer(m.to.stage, m.to.room);
    m.key = moveKey(m);
    std::string bitText;
    for (const uint16_t no : m.bits) {
        bitText += fmt::format("{}{:04X}", bitText.empty() ? "" : " ", no);
    }
    TwiliLog.info("[story] settle key '{}' hops {} bits {}/{} [{}]{}{}", m.key, m.hopList.size(),
        sinceAccept, sinceVisit, bitText, m.transientHint ? " transient layer" : "",
        m.bossDefeated ? " boss defeated" : "");
    detail::onOwnMoveSettled(std::move(p.move), std::max(sinceAccept, sinceVisit), p.copyOf);
}

void Tracker::tick() {
    if (!isSaveLoaded()) {
        if (mHadSave) {
            clear();
        }
        mHadSave = false;
        return;
    }
    mHadSave = true;
    const uint32_t tick = nowTick();

    // Instances end when no event runs any more.
    const bool running = dComIfGp_event_runCheck() != FALSE;
    if (!mInstances.empty() && mInstances.back().running()) {
        Instance& in = mInstances.back();
        if (running) {
            in.mode = dComIfGp_getEvent()->mMode;
            in.eventFlag = dComIfGp_getEvent()->mEventFlag;
        } else {
            in.endedTick = tick != 0 ? tick : 1;
        }
    }
    if (mEventWasRunning && !running) {
        mLastEventEndTick = tick;
    }
    mEventWasRunning = running;

    const bool linkLive =
        local::liveLink() != nullptr && !dComIfGp_isEnableNextStage() && saveTblNo() >= 0;
    if (linkLive && !mLinkWasLive) {
        mStageLoadSeq++;
        mStageLiveTick = tick;
    }
    mLinkWasLive = linkLive;

    const bool pending = dComIfGp_isEnableNextStage();
    if (pending && !mPrevPending) {
        PendingMove dep;
        dep.departedAt = Clock::now();
        if (captureDeparture(dep)) {
            if (mMove && mMove->arrived && mMove->move.hops < kMaxHops) {
                // An arrival cutscene that relocates again: one move, final destination.
                MoveRecord& m = mMove->move;
                m.to = dep.move.to;
                m.hops++;
                mMove->sawUnload = mMove->leftOld = mMove->arrived = false;
                // Each hop gets its own arrival timeout: arrival scenes can run a minute.
                mMove->departedAt = dep.departedAt;
                TwiliLog.info("[story] move continues ({} hops) to {}", m.hops, entranceText(m.to));
            } else {
                dep.move.id = newMoveId();
                mMove = std::move(dep);
            }
        }
    }
    mPrevPending = pending;

    if (!mMove) {
        return;
    }
    PendingMove& p = *mMove;
    if (!p.arrived) {
        // The old Link and save table are gone before the new stage clears the request.
        if (pending || saveTblNo() < 0) {
            p.sawUnload = true;
        }
        if (p.sawUnload && !pending) {
            p.leftOld = true;
        }
        if (p.leftOld && linkLive) {
            recordArrival();
        } else if (Clock::now() - p.departedAt > kArrivalTimeout) {
            TwiliLog.warn("[story] move to {} never arrived", p.move.to.stage);
            mMove.reset();
        }
        return;
    }
    const uint32_t dt = tick - p.lastTick;
    p.lastTick = tick;
    p.quiet = running || pending ? 0 : p.quiet + dt;
    if (pending) {
        return;  // a load that did not qualify as a hop: wait for it to resolve
    }
    if ((p.quiet >= kSettleQuietTicks || Clock::now() - p.arrivedAt > kSettleMax) &&
        sync::ownStateSettled())
    {
        settle();
    }
}

std::string Tracker::describe() const {
    std::string s = fmt::format("loads {} sinceLoad {} quiet {} bits {}", mStageLoadSeq,
        ticksSinceLoad(), quietTicks(), mLocalEventBitsSet);
    if (const Instance* in = lastInstance()) {
        s += fmt::format(" last #{} '{}' map {} {}", in->id, in->name, in->mapToolId,
            in->running() ? "running" : "ended");
    }
    if (mMove) {
        s += fmt::format(" move {} -> {} {}", mMove->move.from.stage, mMove->move.to.stage,
            mMove->arrived ? "arrived" : "loading");
    }
    return s;
}

namespace {

std::vector<fopAc_ac_c*> s_found;

void* collectEventTags(void* proc, void*) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const s16 name = fopAcM_GetName(actor);
    if (name == fpcNm_TAG_EVENT_e || name == fpcNm_TAG_EVT_e || name == fpcNm_TAG_EVTAREA_e) {
        s_found.push_back(actor);
    }
    return nullptr;
}

std::string exitText(stage_scls_info_dummy_class* scls, uint8_t exitId) {
    if (exitId == 0xFF) {
        return "-";
    }
    if (scls == nullptr || exitId >= scls->num) {
        return fmt::format("{}?", exitId);
    }
    stage_scls_info_class& e = scls->m_entries[exitId];
    return fmt::format("{}:{} r{} p{} l{}", exitId, std::string(e.mStage, strnlen(e.mStage, 8)),
        e.mRoom, e.mStart, dStage_sclsInfo_getSceneLayer(&e));
}

void dumpMapEvents(
    const char* what, dStage_MapEventInfo_c* info, stage_scls_info_dummy_class* scls) {
    if (info == nullptr) {
        TwiliLog.info("[story] dump {}: no map events", what);
        return;
    }
    for (int i = 0; i < info->num; i++) {
        dStage_MapEvent_dt_c* dt = &info->m_entries[i];
        const bool maptool = dt->type == dStage_MapEvent_dt_TYPE_MAPTOOLCAMERA;
        TwiliLog.info("[story] dump {} map {} type {} '{}' sw {} next {} prio {} exit {} skip {}{} "
                      "scut sw {} type {}",
            what, dt->field_0x4, dt->type, mapEventName(dt), dt->switch_no, dt->field_0x5,
            dt->priority, exitText(scls, dt->field_0x7), exitText(scls, dt->field_0x9),
            maptool ? fmt::format(" camExit {}", exitText(scls, dt->data.maptool.field_0x17)) :
                      std::string{},
            dStage_MapEvent_dt_c_getEventSCutSW(dt), dStage_MapEvent_dt_c_getEventSCutType(dt));
    }
}

void dumpExits(const char* what, stage_scls_info_dummy_class* scls) {
    for (int i = 0; scls != nullptr && i < scls->num; i++) {
        stage_scls_info_class& e = scls->m_entries[i];
        TwiliLog.info("[story] dump {} exit {} -> {} room {} point {} layer {}", what, i,
            std::string(e.mStage, strnlen(e.mStage, 8)), e.mRoom, e.mStart,
            dStage_sclsInfo_getSceneLayer(&e));
    }
}

}  // namespace

void dumpStageEvents() {
    const int stay = dComIfGp_roomControl_getStayNo();
    TwiliLog.info("[story] dump stage {} room {} layer {} point {}", currentStageName(), stay,
        dComIfG_play_c::getLayerNo(0), dComIfGp_getStartStagePoint());
    dStage_roomDt_c* room = dComIfGp_roomControl_getStatusRoomDt(stay);
    if (room != nullptr) {
        dumpMapEvents("room", room->getMapEventInfo(), room->getSclsInfo());
    }
    dumpMapEvents("stage", dComIfGp_getStage()->getMapEventInfo(), dComIfGp_getStageSclsInfo());
    if (room != nullptr) {
        dumpExits("room", room->getSclsInfo());
        // PLYR entries: point, start mode and the map event Link starts on arrival there.
        if (stage_actor_class* plyr = room->getPlayer()) {
            for (int i = 0; i < plyr->num; i++) {
                const fopAcM_prmBase_class& base = plyr->m_entries[i].base;
                const u32 prm = base.parameters;
                const cXyz pos = base.position;
                const csXyz angle = base.angle;
                TwiliLog.info("[story] dump plyr point {} mode {} start event {} at ({:.0f} {:.0f} "
                              "{:.0f})",
                    static_cast<u8>(angle.z), (prm >> 12) & 0x1F, prm >> 24, pos.x, pos.y, pos.z);
            }
        }
    }
    dumpExits("stage", dComIfGp_getStageSclsInfo());
    s_found.clear();
    fopAcM_Search(&collectEventTags, nullptr);
    for (fopAc_ac_c* actor : s_found) {
        const s16 name = fopAcM_GetName(actor);
        if (name == fpcNm_TAG_EVENT_e) {
            auto* tag = static_cast<daTag_Event_c*>(actor);
            TwiliLog.info("[story] dump tag_event room {} pos ({:.0f} {:.0f} {:.0f}) scale ({:.0f} "
                          "{:.0f}) event {} swbit {} swbit2 {} type {} area 0x{:X} valid {} "
                          "invalid {} terms {} action {}",
                fopAcM_GetRoomNo(actor), actor->current.pos.x, actor->current.pos.y,
                actor->current.pos.z, actor->scale.x, actor->scale.y, tag->getEventNo(),
                tag->getSwbit(), tag->getSwbit2(), tag->getType(), tag->getAreaType(),
                tag->getValidEventFlag(), tag->getInvalidEventFlag(), tag->arrivalTerms() != FALSE,
                tag->mAction);
        } else {
            TwiliLog.info(
                "[story] dump tag profile 0x{:X} room {} pos ({:.0f} {:.0f} {:.0f}) param "
                "0x{:08X} mapTool {} event {}",
                name, fopAcM_GetRoomNo(actor), actor->current.pos.x, actor->current.pos.y,
                actor->current.pos.z, fopAcM_GetParam(actor), actor->eventInfo.getMapToolId(),
                actor->eventInfo.getEventId());
        }
    }
}

}  // namespace twili::story
