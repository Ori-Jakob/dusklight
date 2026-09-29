// STORY_MOVE: a story relocation teammates elsewhere may follow, loading with their own flags.

#include "story/StoryState.hpp"

#include "core/Config.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "story/StoryLog.hpp"
#include "sync/WorldSync.hpp"
#include "teleport/Teleport.hpp"
#include "ui/StoryPrompt.hpp"
#include "ui/Toasts.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "d/d_stage.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace twili::story {
namespace detail {
namespace {

// A move this close to our own story arrival is only a menu row, so nobody ping-pongs.
constexpr auto kOwnArrivalGrace = std::chrono::seconds(10);
// How long an offered pop-up waits for the player to be free.
constexpr auto kCalmTimeout = std::chrono::minutes(5);
// How long an accepted follow waits for the player to be free.
constexpr auto kLoadWaitTimeout = std::chrono::minutes(5);
constexpr auto kLoadArrivalTimeout = std::chrono::seconds(45);
// Before M_077 synced levels decide the spawn form; a mismatch means a merge is pending.
constexpr auto kMergeWait = std::chrono::seconds(10);
// A cutscene layer (TWgate's layer 10) is only right while its cutscene is current.
constexpr auto kExplicitLayerFresh = std::chrono::seconds(120);
// A move replayed from the server cache older than this is a menu row, not a pop-up.
constexpr auto kCachedMovePromptAge = std::chrono::minutes(10);
// The consistency check waits this long after a stage load, with no event running.
constexpr uint32_t kConsistencySettleTicks = 150;
constexpr int kMaxInconsistentDeclines = 3;

State s_state;

const char* currentStage() {
    const char* s = dComIfGp_getStartStageName();
    return s != nullptr ? s : "";
}

std::string withName(const char* tmpl, const std::string& name) {
    std::string s = tmpl;
    const size_t at = s.find("{name}");
    if (at != std::string::npos) {
        s.replace(at, 6, name);
    }
    return s;
}

void toast(const std::string& text) {
    ui::toast("Story", text, ui::kToastStory, 5000);
}

std::string selfName() {
    if (const Client* self = Session::instance().selfClient();
        self != nullptr && !self->name.empty())
    {
        return self->name;
    }
    return config::getString(config::Var::DisplayName);
}

// The entrance, layer and form a follower of `m` loads.
CatchUpPlan followPlan(const MoveRecord& m) {
    CatchUpPlan p;
    p.kind = CatchUpPlan::Kind::Entrance;
    p.entrance = m.to;
    p.moveId = m.id;
    p.place = placeName(m);
    const StoryMoveDef* def = curatedMove(m.curated);
    if (def != nullptr && def->followPoint >= 0) {
        p.entrance.point = def->followPoint;
    }
    p.layerArg = -1;
    if (def != nullptr && def->keepExplicitLayer && m.to.layerArg >= 0 &&
        Clock::now() - m.at < kExplicitLayerFresh)
    {
        const auto& clients = Session::instance().clients();
        const auto it = clients.find(m.originClientId);
        if (it != clients.end() && it->second.online &&
            std::strncmp(it->second.stageName, m.to.stage, sizeof(m.to.stage)) == 0 &&
            it->second.layerNo == m.to.layer)
        {
            p.layerArg = m.to.layerArg;
        }
    }
    p.form = def != nullptr && def->form != Form::Any ? def->form :
             m.wolfAfter                              ? Form::Wolf :
                                                        Form::Human;
    const std::string name = m.originName.empty() ? std::string("A teammate") : m.originName;
    p.title = fmt::format("Follow {} to {}", name, p.place);
    p.reason = def != nullptr ? withName(def->prompt, name) + "." :
                                fmt::format("{} was moved by a story event.", name);
    return p;
}

CatchUpPlan segmentPlan(const StorySegment& seg) {
    CatchUpPlan p;
    p.kind = CatchUpPlan::Kind::Entrance;
    p.entrance = seg.canonical;
    p.form = seg.form;
    p.place = seg.place;
    p.segment = seg.id;
    p.title = fmt::format("Catch up: go to {}", seg.place);
    p.reason = fmt::format("Your story says {}.", seg.text);
    return p;
}

bool stageIsDungeon() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info != nullptr && dStage_stagInfo_GetSTType(info) == ST_DUNGEON;
}

std::string movePromptText(const MoveRecord& m, bool title) {
    const std::string name = m.originName.empty() ? std::string("A teammate") : m.originName;
    const StoryMoveDef* def = curatedMove(m.curated);
    if (title) {
        return def != nullptr ? withName(def->prompt, name) :
                                fmt::format("{} was moved by a story event", name);
    }
    const CatchUpPlan f = followPlan(m);
    return fmt::format("{} was taken to {} by a story event.\nFollow? You will arrive as a {}.",
        name, f.place, formName(f.form));
}

void finishLoad(LoadPhase phase, std::string reason, std::string message) {
    LoadState& l = s_state.load;
    l.phase = phase;
    l.reason = std::move(reason);
    l.message = std::move(message);
    TwiliLog.info("[story] load {}{}{} - {}", loadPhaseName(phase), l.reason.empty() ? "" : " ",
        l.reason, l.message);
    if (phase == LoadPhase::Failed) {
        toast(l.message);
    }
}

bool startLoad(const Entrance& e, int8_t layerArg, Form form, const std::string& moveId,
    const std::string& source, bool needSync) {
    LoadState& l = s_state.load;
    if (loadActive()) {
        return false;
    }
    l = LoadState{};
    l.entrance = e;
    l.layerArg = layerArg;
    l.form = form;
    l.moveId = moveId;
    l.source = source;
    l.needSync = needSync;
    l.requestedAt = Clock::now();
    if (!moveId.empty() && s_state.team.valid && s_state.team.move.id == moveId) {
        l.waitMergeFrom = s_state.team.move.originClientId;
    }
    // An unknown point is fatal in dStage_playerInit.
    if (!e.valid() || !local::isKnownEntrance(e.stage, e.room, e.point)) {
        finishLoad(LoadPhase::Failed, "bad-destination",
            fmt::format("Cannot load {} room {} point {}.", e.stage, e.room, e.point));
        return false;
    }
    l.phase = LoadPhase::Waiting;
    if (!moveId.empty()) {
        s_state.answeredMoves.insert(moveId);
    }
    closePrompt();
    TwiliLog.info("[story] load requested {} room {} point {} layer arg {} (predicted layer {}), "
                  "expected {}, {}",
        e.stage, e.room, e.point, layerArg,
        dComIfG_play_c::getLayerNo_common(e.stage, e.room, layerArg), formName(form), source);
    return true;
}

// A real start point for dStage_playerInit; room param 0 keeps the entry's start mode.
void requestEntranceLoad(const LoadState& l) {
    dComIfGs_setRestartRoomParam(0);
    dComIfGp_setNextStage(
        l.entrance.stage, l.entrance.point, l.entrance.room, l.layerArg, 0.0f, 0, 1, 0, 0, 1, 0);
}

void tickLoadWaiting() {
    LoadState& l = s_state.load;
    const auto now = Clock::now();
    if (l.needSync && !sync::enabled()) {
        finishLoad(LoadPhase::Failed, "disabled", "Follow cancelled: world sync is off.");
        return;
    }
    if (now - l.requestedAt > kLoadWaitTimeout) {
        finishLoad(LoadPhase::Failed, "timeout",
            "Follow cancelled. You can catch up later from the Twili-Together window.");
        return;
    }
    const char* why = local::localTeleportBlocker();
    if (why == nullptr && !teleport::idle()) {
        why = "pending";
    }
    // Through the fade Link could void out from the air or a hazard floor.
    if (why == nullptr && !local::onSafeFloor(local::liveLink())) {
        why = "footing";
    }
    // The destination's layer and our spawn form come from flags a cached move can arrive ahead of.
    if (why == nullptr && l.needSync && !sync::ownStateSettled()) {
        why = "flags";
    }
    if (why != nullptr) {
        l.reason = why;
        return;
    }
    if (!l.formChecked && l.form != Form::Any) {
        const bool wantWolf = l.form == Form::Wolf;
        if (dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
            // Transforming unlocked: the per-player status world sync never touches decides.
            dComIfGs_setTransformStatus(wantWolf ? 1 : 0);
        } else if (predictSpawnWolf(l.entrance) != wantWolf) {
            if (l.waitMergeFrom != 0 && l.mergeWaitUntil == Clock::time_point{}) {
                l.mergeWaitUntil = now + kMergeWait;
                l.mergeBaseline = sync::mergeCount(l.waitMergeFrom);
                TwiliLog.info("[story] load waits for {}'s world state (predicted {})",
                    teleport::clientName(l.waitMergeFrom), wantWolf ? "human" : "wolf");
                l.reason = "flags";
                return;
            }
            if (l.waitMergeFrom != 0 && now < l.mergeWaitUntil &&
                sync::mergeCount(l.waitMergeFrom) == l.mergeBaseline)
            {
                l.reason = "flags";
                return;
            }
            if (l.waitMergeFrom != 0 && predictSpawnWolf(l.entrance) == wantWolf) {
                l.formChecked = true;
                return;  // the merge fixed it; load next tick
            }
            // Never write levels here: that would edit story flags and spread them.
            TwiliLog.warn("[story] load expects a {} but our flags spawn a {}", formName(l.form),
                wantWolf ? "human" : "wolf");
            toast(fmt::format("Your story flags differ from your team's; you may arrive as a {}.",
                wantWolf ? "human" : "wolf"));
        }
        l.formChecked = true;
    }
    requestEntranceLoad(l);
    l.phase = LoadPhase::Loading;
    l.reason.clear();
    l.loadStartedAt = now;
    TwiliLog.info("[story] load started {} room {} point {} layer arg {}", l.entrance.stage,
        l.entrance.room, l.entrance.point, l.layerArg);
}

void tickLoadLoading() {
    LoadState& l = s_state.load;
    const auto now = Clock::now();
    const int saveTblNo = Session::instance().currentSaveTblNo();
    const bool pending = dComIfGp_isEnableNextStage();
    // While the old stage runs this has the last word before phase_1 reads the request.
    if (pending && !l.leftOld) {
        const bool ours = std::strncmp(dComIfGp_getNextStageName(), l.entrance.stage, 8) == 0 &&
                          dComIfGp_getNextStagePoint() == l.entrance.point &&
                          dComIfGp_getNextStageRoomNo() == l.entrance.room &&
                          dComIfGp_getNextStageLayer() == l.layerArg;
        if (!ours && dComIfGp_getNextStagePoint() == -1) {
            TwiliLog.warn("[story] load replaced by a restart; restoring it");
            requestEntranceLoad(l);
        }
    }
    if (pending || saveTblNo < 0) {
        l.sawUnload = true;
    }
    if (l.sawUnload && !pending) {
        l.leftOld = true;
    }
    daAlink_c* link = local::liveLink();
    if (l.leftOld && !pending && saveTblNo >= 0 && link != nullptr) {
        const char* stage = currentStage();
        if (std::strncmp(stage, l.entrance.stage, 8) != 0) {
            finishLoad(LoadPhase::Failed, "diverted",
                fmt::format("The story load ended in {} instead of {}.", stage, l.entrance.stage));
            return;
        }
        const bool wolf = link->checkWolf() != 0;
        TwiliLog.info("[story] load arrived {} room {} point {} layer {} {} (expected {}, {})",
            stage, dComIfGp_roomControl_getStayNo(), dComIfGp_getStartStagePoint(),
            dComIfG_play_c::getLayerNo(0), wolf ? "wolf" : "human", formName(l.form), l.source);
        tracker().setLastOwnStoryArrival(now);
        if (!l.moveId.empty() && s_state.team.valid && s_state.team.move.id == l.moveId) {
            s_state.team.satisfied = true;
        }
        if (l.form != Form::Any && wolf != (l.form == Form::Wolf)) {
            finishLoad(LoadPhase::Failed, "form",
                fmt::format("You arrived as a {} (your story expects a {}).",
                    wolf ? "wolf" : "human", formName(l.form)));
        } else {
            finishLoad(LoadPhase::Arrived, "", "Caught up with the story.");
        }
        l.chainOpen = true;
    } else if (now - l.loadStartedAt > kLoadArrivalTimeout) {
        finishLoad(LoadPhase::Failed, "stuck", "The destination did not finish loading.");
    }
}

void showPrompt(PromptKind kind) {
    PromptState& pr = s_state.prompt;
    pr.kind = kind;
    pr.showing = false;
    pr.waitingCalm = true;
    pr.waitingSince = Clock::now();
    pr.moveId = kind == PromptKind::Move ? s_state.team.move.id : std::string{};
    TwiliLog.info("[story] prompt {} offered", promptKindName(kind));
}

bool joining() {
    const JoinState j = s_state.join.state;
    return j == JoinState::Waiting || j == JoinState::Ordered || j == JoinState::Running;
}

void offerTeamMove() {
    TeamMove& team = s_state.team;
    PromptState& pr = s_state.prompt;
    team.offered = true;
    const auto own = tracker().lastOwnStoryArrival();
    const bool nearOwn = own && team.move.at < *own + kOwnArrivalGrace;
    const bool stale = team.fromCache && Clock::now() - team.move.at > kCachedMovePromptAge;
    const std::string title = movePromptText(team.move, true);
    if (team.move.strong() && config::getBool(config::Var::StoryPrompts) && !nearOwn && !stale &&
        pr.kind == PromptKind::None)
    {
        showPrompt(PromptKind::Move);
        if (local::localTeleportBlocker() != nullptr || ui::anyDocumentVisible()) {
            toast(title + ". You can follow once you are free.");
        }
        return;
    }
    TwiliLog.info("[story] move offered as a menu row ({})",
        nearOwn             ? "our own story moved at the same time" :
        stale               ? "old" :
        !team.move.strong() ? "weak" :
                              "prompts off");
    toast(fmt::format("{} ({}). Catch up to story in the Twili-Together window follows.", title,
        placeName(team.move)));
}

void checkConsistency() {
    PromptState& pr = s_state.prompt;
    const CatchUpPlan& plan = s_state.plan;
    if (!plan.inconsistent) {
        return;
    }
    TwiliLog.info("[story] inconsistent: segment {} does not allow {} (plan {} - {})", plan.segment,
        currentStage(), catchUpKindName(plan.kind), plan.reason);
    if (!sync::enabled() || pr.kind != PromptKind::None || plan.kind == CatchUpPlan::Kind::None) {
        return;
    }
    if (pr.inconsistentDeclines >= kMaxInconsistentDeclines) {
        TwiliLog.info(
            "[story] catch-up not offered again (declined {} times)", pr.inconsistentDeclines);
    } else if (!config::getBool(config::Var::StoryPrompts)) {
        toast("Your story has moved on. Catch up to story in the Twili-Together window.");
    } else {
        showPrompt(PromptKind::Inconsistent);
    }
}

void presentPrompt() {
    PromptState& pr = s_state.prompt;
    const TeamMove& team = s_state.team;
    pr.waitingCalm = false;
    pr.showing = true;
    ui::StoryPromptProps props;
    if (pr.kind == PromptKind::Move) {
        props.title = ui::escapeRml(movePromptText(team.move, true));
        std::string body = ui::escapeRml(movePromptText(team.move, false));
        for (size_t at = body.find('\n'); at != std::string::npos; at = body.find('\n')) {
            body.replace(at, 1, "<br/>");
        }
        props.bodyRml = body;
        props.acceptLabel = "Follow";
        props.onAccept = [] { answerPrompt(PromptAnswer::Follow); };
    } else {
        const CatchUpPlan& p = s_state.plan;
        props.title = "Your story has moved on";
        props.bodyRml = ui::escapeRml(p.reason) + "<br/>" +
                        ui::escapeRml(fmt::format("Catch up now? {}.", p.title));
        if (p.kind == CatchUpPlan::Kind::Entrance && p.form != Form::Any) {
            props.bodyRml +=
                ui::escapeRml(fmt::format(" You will arrive as a {}.", formName(p.form)));
        }
        props.acceptLabel = "Catch up";
        props.onAccept = [] { answerPrompt(PromptAnswer::CatchUp); };
    }
    if (stageIsDungeon()) {
        props.bodyRml += "<br/>Doors and switches that only stay open while you are inside reset "
                         "when you leave; small keys, chests and maps are kept.";
    }
    props.declineLabel = "Not now";
    props.onDecline = [] { answerPrompt(PromptAnswer::Decline); };
    TwiliLog.info("[story] prompt {} shown", promptKindName(pr.kind));
    ui::showStoryPrompt(std::move(props));
}

void resetState() {
    closePrompt();
    const std::string forced = s_state.forcedBlocker;
    s_state = State{};
    s_state.forcedBlocker = forced;
}

}  // namespace

State& state() {
    return s_state;
}

std::string ago(Clock::time_point t) {
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t).count();
    if (sec < 60) {
        return "just now";
    }
    if (sec < 3600) {
        return fmt::format("{} min ago", sec / 60);
    }
    return fmt::format("{} h ago", sec / 3600);
}

std::string moveTitle(const MoveRecord& m) {
    return movePromptText(m, true);
}

void sendPacket(nlohmann::json packet) {
    packet["sv"] = kSyncVersion;
    sync::stampPacket(packet, false);
    Session::instance().send(packet);
}

bool acceptsPacket(const nlohmann::json& packet, bool needCutsceneSync) {
    const Session& session = Session::instance();
    if (!sync::enabled() || !isSaveLoaded()) {
        return false;
    }
    if (needCutsceneSync && !session.roomState().cutsceneSync) {
        return false;
    }
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == session.selfClientId()) {
        return false;
    }
    if (packet.value("senderSessionKey", std::string{}) == session.sessionKey()) {
        return false;
    }
    if (packet.value("teamId", std::string{}) != session.selfTeamId()) {
        return false;
    }
    // Another game version: its story flags are not merged here.
    if (packet.value("layout", std::string{}) != sync::localLayout()) {
        return false;
    }
    if (packet.value("sv", -1) != kSyncVersion) {
        static std::set<uint32_t> s_logged;
        if (s_logged.insert(id).second) {
            TwiliLog.warn("[story] dropping {} from client {} (story version {} != {})",
                packet.value("type", std::string{}), id, packet.value("sv", -1), kSyncVersion);
        }
        return false;
    }
    return true;
}

void onOwnMoveSettled(MoveRecord move, uint32_t localBitsDuring, uint32_t copyOf) {
    move.qual = qualify(move, localBitsDuring);
    move.at = Clock::now();
    move.originClientId = Session::instance().selfClientId();
    move.originName = selfName();
    const char* curated = move.curated >= 0 ? kStoryMoves[move.curated].id : "-";
    const bool syncing = sync::enabled();
    const char* outcome = copyOf == kFollowChainCopy ? "not sent (part of our follow)" :
                          copyOf != 0                ? "not sent (pull-in copy)" :
                          move.qual == 0             ? "ignored (repeatable)" :
                          !syncing                   ? "not sent (world sync off)" :
                                                       "sent";
    TwiliLog.info("[story] move {} room {} -> {} room {} point {} layer {} {} curated {} qual "
                  "0x{:X} hops {} {}",
        move.from.stage, move.from.room, move.to.stage, move.to.room, move.to.point, move.to.layer,
        move.wolfAfter ? "wolf" : "human", curated, move.qual, move.hops, outcome);
    if (move.qual != 0 || copyOf != 0) {
        tracker().setLastOwnStoryArrival(move.at);
    }
    TeamMove& team = s_state.team;
    if (team.valid && move.to.sameStageRoom(team.move.to.stage, team.move.to.room)) {
        team.satisfied = true;
    }
    if (copyOf == 0) {
        storylog::note(move);
    }
    if (copyOf != 0 || move.qual == 0 || !syncing) {
        return;
    }
    nlohmann::json packet = move.toJson();
    packet["type"] = "STORY_MOVE";
    packet["ph"] = "arrive";
    packet["name"] = move.originName;
    if (const StorySegment* seg = activeSegment()) {
        packet["seg"] = seg->id;
    }
    sendPacket(std::move(packet));
    s_state.movesSent++;
    s_state.lastMoveCurated = move.curated >= 0 ? curated : "";
    s_state.lastMoveQual = move.qual;
    s_state.lastMove = move;
}

void handleStoryMove(const nlohmann::json& packet) {
    if (!acceptsPacket(packet, false) || packet.value("ph", std::string{}) != "arrive") {
        return;
    }
    MoveRecord m = MoveRecord::fromJson(packet);
    if (!m.to.valid()) {
        TwiliLog.warn("[story] STORY_MOVE from client {} has no usable destination",
            packet.value("clientId", 0u));
        return;
    }
    m.originClientId = packet.value("clientId", 0u);
    m.originName = packet.value("name", std::string{}).substr(0, 32);
    if (m.originName.empty()) {
        m.originName = teleport::clientName(m.originClientId);
    }
    const bool fromCache = packet.value("fromCache", false);
    const int64_t ageMs = std::max<int64_t>(0, packet.value("ageMs", int64_t{0}));
    m.at = Clock::now() - std::chrono::milliseconds(ageMs);
    // Our copy of the curated row's text, even if the sender's build has no such row.
    if (m.curated < 0) {
        m.curated = matchCuratedMove(m);
    }
    if (m.key.empty()) {
        m.key = moveKey(m);
    }
    storylog::note(m);

    s_state.movesReceived++;
    s_state.lastMoveCurated = m.curated >= 0 ? kStoryMoves[m.curated].id : "";
    s_state.lastMoveQual = m.qual;
    s_state.lastMoveFromCache = fromCache;
    s_state.lastMove = m;
    s_state.clientMoves[m.originClientId] = {
        m.curated >= 0 ? withName(kStoryMoves[m.curated].prompt, m.originName) :
                         fmt::format("{} moved to {}", m.originName, placeName(m)),
        m.at};

    TeamMove& team = s_state.team;
    if (team.valid && team.move.id == m.id) {
        return;  // the cache replay of a move we have
    }
    if (team.valid && m.at < team.move.at) {
        TwiliLog.info("[story] move {} from {} is older than the team's latest; kept as a row",
            m.id, m.originName);
        return;
    }
    team = TeamMove{};
    team.move = m;
    team.valid = true;
    team.fromCache = fromCache;
    // Already there, or we watched the same cutscene (a pull-in copy follows its exit too).
    const JoinInfo& j = s_state.join;
    const bool here = m.to.sameStageRoom(currentStage(), dComIfGp_roomControl_getStayNo());
    const bool watched = j.originClientId == m.originClientId && j.eventName == m.event.name &&
                         (j.state == JoinState::Running || j.state == JoinState::Ended);
    team.satisfied = here || watched;
    if (s_state.answeredMoves.count(m.id) != 0) {
        team.declined = !team.satisfied;
        team.offered = true;
    }
    TwiliLog.info("[story] move received from {} ({}): {} room {} -> {} room {} point {} layer {} "
                  "curated {} qual 0x{:X}{}{}",
        m.originName, m.originClientId, m.from.stage, m.from.room, m.to.stage, m.to.room,
        m.to.point, m.to.layer, m.curated >= 0 ? kStoryMoves[m.curated].id : "-", m.qual,
        fromCache ? fmt::format(" (cached, {} ms old)", ageMs) : std::string{},
        team.satisfied ? (here ? " - already there" : " - watched it") : "");
}

bool followChainOpen() {
    return s_state.load.phase == LoadPhase::Arrived && s_state.load.chainOpen;
}

void tickLoad() {
    LoadState& l = s_state.load;
    switch (l.phase) {
    case LoadPhase::Waiting:
        tickLoadWaiting();
        return;
    case LoadPhase::Loading:
        tickLoadLoading();
        return;
    case LoadPhase::Arrived:
    case LoadPhase::Failed:
        // The destination's own arrival cutscenes are over.
        if (l.chainOpen && tracker().quietTicks() >= 45 && !dComIfGp_isEnableNextStage() &&
            !tracker().movePending())
        {
            l.chainOpen = false;
        }
        return;
    default:
        return;
    }
}

void computeCatchUpPlan() {
    const Session& session = Session::instance();
    CatchUpPlan p;
    const StorySegment* seg = activeSegment();
    const char* stage = currentStage();
    p.segment = seg != nullptr ? seg->id : "";
    p.inconsistent = seg != nullptr && !segmentAllows(*seg, stage);
    p.title = "Catch up to story";

    if (session.isConnected() && !session.roomState().syncWorldState) {
        p.reason = "World sync is off: your story is your own.";
        s_state.plan = std::move(p);
        return;
    }
    // 1. The team's latest move, if our own story has not moved on since.
    const TeamMove& team = s_state.team;
    const auto own = tracker().lastOwnStoryArrival();
    if (sync::enabled() && team.valid && !team.satisfied && (!own || team.move.at > *own) &&
        (seg == nullptr || segmentAllows(*seg, team.move.to.stage)))
    {
        CatchUpPlan f = followPlan(team.move);
        f.segment = p.segment;
        f.inconsistent = p.inconsistent;
        s_state.plan = std::move(f);
        return;
    }
    if (!p.inconsistent) {
        p.reason = "Nothing to catch up on.";
        s_state.plan = std::move(p);
        return;
    }
    // 2. The segment's canonical entrance: needs no network, so a stuck save is repaired offline.
    if (seg->canonical.valid()) {
        CatchUpPlan s = segmentPlan(*seg);
        s.inconsistent = true;
        s_state.plan = std::move(s);
        return;
    }
    // 3. A teammate standing where the story allows, through teleport-to-player.
    std::string mate;
    for (const auto& [id, c] : session.clients()) {
        if (c.self || !c.online || !c.isSaveLoaded || !session.isTeammate(c) ||
            !segmentAllows(*seg, c.stageName))
        {
            continue;
        }
        mate = c.name;
        if (session.roomState().teleportMode && teleport::blockCode(id) == nullptr) {
            p.kind = CatchUpPlan::Kind::TeleportToPlayer;
            p.clientId = id;
            p.form = seg->form;
            p.place = local::mapName(c.stageName, c.roomNo);
            p.title = fmt::format("Catch up: teleport to {}", c.name);
            p.reason = fmt::format("Your story says {}.", seg->text);
            s_state.plan = std::move(p);
            return;
        }
    }
    // 4. Nothing known.
    p.reason = fmt::format("Your story says {}. No known way back yet: ask {} to stand on solid "
                           "ground in {} and use Teleport.",
        seg->text, mate.empty() ? std::string("a teammate") : mate, seg->place);
    s_state.plan = std::move(p);
}

const char* catchUpBlockCode() {
    const CatchUpPlan& p = s_state.plan;
    if (p.kind == CatchUpPlan::Kind::None) {
        return "nothing";
    }
    if (loadActive() || !teleport::idle()) {
        return "pending";
    }
    if (p.kind == CatchUpPlan::Kind::TeleportToPlayer) {
        return teleport::blockCode(p.clientId);
    }
    if (!s_state.forcedBlocker.empty()) {
        return s_state.forcedBlocker.c_str();
    }
    return local::localTeleportBlocker();
}

bool startCatchUpPlan() {
    const CatchUpPlan p = s_state.plan;
    if (catchUpBlockCode() != nullptr) {
        return false;
    }
    TwiliLog.info("[story] catch up to story: {} ({})", p.title, p.reason);
    if (p.kind == CatchUpPlan::Kind::TeleportToPlayer) {
        return teleport::request(p.clientId);
    }
    return startLoad(p.entrance, p.layerArg, p.form, p.moveId,
        p.moveId.empty() ? "segment " + p.segment : "move " + p.moveId, !p.moveId.empty());
}

void closePrompt() {
    PromptState& pr = s_state.prompt;
    if (pr.showing) {
        ui::closeStoryPrompt();
    }
    pr.kind = PromptKind::None;
    pr.showing = false;
    pr.waitingCalm = false;
    pr.moveId.clear();
}

void answerPrompt(PromptAnswer answer) {
    const PromptKind kind = s_state.prompt.kind;
    closePrompt();
    const char* answerName = answer == PromptAnswer::Follow  ? "follow" :
                             answer == PromptAnswer::CatchUp ? "catchup" :
                                                               "decline";
    TwiliLog.info("[story] prompt {} answered {}", promptKindName(kind), answerName);
    TeamMove& team = s_state.team;
    if (kind == PromptKind::Move) {
        if (team.valid) {
            s_state.answeredMoves.insert(team.move.id);
        }
        if (answer == PromptAnswer::Decline) {
            team.declined = true;
            ui::toast("Story", "You can catch up later: Twili-Together window, Players tab.",
                ui::kToastStory, 4000);
        } else if (team.valid) {
            const CatchUpPlan f = followPlan(team.move);
            startLoad(f.entrance, f.layerArg, f.form, f.moveId, "move " + f.moveId, true);
        }
    } else if (kind == PromptKind::Inconsistent) {
        if (answer == PromptAnswer::Decline) {
            s_state.prompt.inconsistentDeclines++;
        } else {
            computeCatchUpPlan();
            startCatchUpPlan();
        }
    }
}

void tickPrompt() {
    PromptState& pr = s_state.prompt;
    TeamMove& team = s_state.team;
    const auto now = Clock::now();
    const bool syncing = sync::enabled();

    // A pop-up that no longer applies goes away.
    if (pr.kind == PromptKind::Move &&
        (!team.valid || team.move.id != pr.moveId || team.satisfied || !syncing || loadActive()))
    {
        closePrompt();
    } else if (pr.kind == PromptKind::Inconsistent &&
               (!s_state.plan.inconsistent || !syncing || loadActive()))
    {
        closePrompt();
    }

    if (team.valid && !team.offered && !team.satisfied && !team.declined && syncing) {
        offerTeamMove();
    }

    // Once per stage load, when settled: a player the story left behind is offered to catch up.
    const uint32_t seq = tracker().stageLoadSeq();
    if (seq != pr.checkedLoadSeq && tracker().ticksSinceLoad() >= kConsistencySettleTicks &&
        dComIfGp_event_runCheck() == FALSE && !loadActive() && teleport::idle() && !joining() &&
        !tracker().movePending())
    {
        pr.checkedLoadSeq = seq;
        checkConsistency();
    }

    // An offered pop-up waits for the player to be free.
    if (pr.kind != PromptKind::None && pr.waitingCalm) {
        const bool calm = local::localTeleportBlocker() == nullptr && !ui::anyDocumentVisible() &&
                          !loadActive() && teleport::idle() && !joining();
        if (calm) {
            presentPrompt();
        } else if (now - pr.waitingSince > kCalmTimeout) {
            TwiliLog.info("[story] prompt {} gave up waiting", promptKindName(pr.kind));
            closePrompt();
        }
    }
    if (pr.showing && !ui::storyPromptShowing()) {
        // Closed by something else: not answered.
        pr.showing = false;
        pr.kind = PromptKind::None;
    }
}

std::string debugText() {
    const TeamMove& team = s_state.team;
    return fmt::format(
        "tracker [{}] plan {} '{}' seg {} inconsistent {} | team {} {} sat {} decl {} off {} | "
        "prompt {} showing {} waiting {} declines {} | load {} {} | join {} {} | sent {} recv {}",
        tracker().describe(), catchUpKindName(s_state.plan.kind), s_state.plan.title,
        s_state.plan.segment, s_state.plan.inconsistent, team.valid ? team.move.id : "-",
        team.valid ? team.move.to.stage : "", team.satisfied, team.declined, team.offered,
        promptKindName(s_state.prompt.kind), s_state.prompt.showing, s_state.prompt.waitingCalm,
        s_state.prompt.inconsistentDeclines, loadPhaseName(s_state.load.phase), s_state.load.reason,
        joinStateName(s_state.join.state), s_state.join.reason, s_state.movesSent,
        s_state.movesReceived);
}

}  // namespace detail

using namespace detail;

bool loadActive() {
    return s_state.load.phase == LoadPhase::Waiting || s_state.load.phase == LoadPhase::Loading;
}

void onEventAccepted(const dEvt_order_c& order) {
    tracker().onEventAccepted(order);
}

void onStageSaveTableLoaded() {
    tracker().onStageSaveTableLoaded();
}

void noteLocalEventBit(uint16_t no) {
    tracker().noteLocalEventBit(no);
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type == "STORY_MOVE") {
        handleStoryMove(packet);
    } else if (type == "STORY_EVENT") {
        handleStoryEvent(packet);
    } else {
        return false;
    }
    return true;
}

void tick() {
    tracker().tick();
    storylog::tick();
    if (!isSaveLoaded()) {
        // A save switched mid-session must not inherit the old one's story state.
        if (s_state.hadSave) {
            resetState();
        }
        s_state.hadSave = false;
        s_state.plan = CatchUpPlan{};
        return;
    }
    s_state.hadSave = true;
    tickLoad();
    tickJoin();
    computeCatchUpPlan();
    tickPrompt();
}

// A load under way and our last arrival survive a reconnect; team moves come from the cache.
void resetSession() {
    if (s_state.prompt.kind == PromptKind::Move) {
        closePrompt();
    }
    s_state.team = TeamMove{};
    s_state.clientMoves.clear();
    s_state.announcedInstance = 0;
    s_state.joinedBy.clear();
    if (s_state.join.state == JoinState::Waiting || s_state.join.state == JoinState::Ordered) {
        s_state.join = JoinInfo{};
    }
}

void shutdown() {
    // The host closes our dialogs itself.
    s_state = State{};
    tracker().clear();
    storylog::shutdown();
}

}  // namespace twili::story
