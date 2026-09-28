// Asks the target through the server; Link moves if its floor is loaded, else the stage loads.

#include "teleport/Teleport.hpp"

#include "core/Host.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "game/TeamGame.hpp"
#include "story/Story.hpp"
#include "ui/Toasts.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace twili::teleport {
namespace {

using Clock = std::chrono::steady_clock;
using local::isSettledOnGround;
using local::isTeleportableRoom;
using local::liveLink;
using local::localTeleportBlocker;
using local::onSafeFloor;

constexpr auto kAnswerTimeout = std::chrono::seconds(6);
// How long an answered teleport waits for us to leave a menu, cutscene or load, or to land.
constexpr auto kExecuteWindow = std::chrono::seconds(15);
constexpr auto kArrivalTimeout = std::chrono::seconds(45);
constexpr auto kCooldown = std::chrono::seconds(2);
constexpr float kMaxCoord = 1.0e6f;
// The spot is where Link's feet were, so the destination floor must be about that high.
constexpr float kFloorTolerance = 60.0f;
// DEFAULT_START: map event 0 would play the destination's story cutscene (South Faron).
constexpr int kStartEventDefaultStart = 0xCA;

struct Destination {
    char stageName[8] = {};
    int8_t roomNo = -1;
    int8_t layerNo = -1;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int16_t angleY = 0;
};

struct SafeSpot {
    bool valid = false;
    Destination dest;
    Clock::time_point capturedAt{};
};

struct State {
    Status status;
    // Never reset: a stale answer can never match.
    uint32_t nextRequestId = 1;
    uint32_t requestId = 0;
    // Our client id when the request went out; a session reset cancels a request not yet loading.
    uint32_t sessionClientId = 0;
    Destination dest;
    int8_t layerArg = -1;
    bool sawStageUnload = false;
    bool leftOldStage = false;
    Clock::time_point sentAt{}, executeDeadline{}, stageRequestedAt{}, cooldownUntil{};
};

State s_state;
SafeSpot s_safeSpot;

Session& session() {
    return Session::instance();
}

bool validDestination(const Destination& d) {
    return d.stageName[0] != '\0' && d.roomNo >= 0 && d.roomNo < 64 && d.layerNo >= 0 &&
           d.layerNo < 15 && std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z) &&
           std::fabs(d.x) < kMaxCoord && std::fabs(d.y) < kMaxCoord && std::fabs(d.z) < kMaxCoord &&
           isTeleportableRoom(d.stageName, d.roomNo);
}

bool sameScene(const Destination& d) {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && std::strncmp(d.stageName, stage, sizeof(d.stageName)) == 0 &&
           d.layerNo == dComIfG_play_c::getLayerNo(0);
}

// Only the layer our progress picks (or the one we run) loads: another's could spread cutscenes.
std::optional<int8_t> destinationLayerArg(const Destination& d) {
    if (dComIfG_play_c::getLayerNo_common(d.stageName, d.roomNo, -1) == d.layerNo) {
        return -1;
    }
    if (sameScene(d)) {
        return d.layerNo;
    }
    return std::nullopt;
}

std::string layerRefusalMessage(const std::string& name, const Destination& d) {
    return fmt::format(
        "{} is in a version of {} your story progress does not load (layer {}, yours {}).", name,
        d.stageName, d.layerNo, dComIfG_play_c::getLayerNo_common(d.stageName, d.roomNo, -1));
}

bool tryLocalTeleport(const Destination& d) {
    daAlink_c* link = liveLink();
    if (link == nullptr || !sameScene(d) || !isSettledOnGround(link)) {
        return false;
    }
    dBgS_LinkGndChk gndChk;
    cXyz probe(d.x, d.y + kFloorTolerance, d.z);
    gndChk.SetPos(&probe);
    dBgS& bg = dComIfG_Bgsp();
    const f32 floorY = bg.GroundCross(&gndChk);
    if (floorY == -G_CM3D_F_INF || std::fabs(floorY - d.y) > kFloorTolerance) {
        return false;
    }
    // Another room's floor, or an actor's, means the destination room is not loaded here.
    if (bg.GetRoomId(gndChk) != d.roomNo || bg.ChkMoveBG_NoDABg(gndChk)) {
        return false;
    }
    const cXyz dest(d.x, floorY, d.z);
    const cXyz delta = dest - link->current.pos;
    // Forced: outside an event the call does nothing otherwise.
    link->setPlayerPosAndAngle(&dest, d.angleY, TRUE);
    link->speedF = 0.0f;
    link->mNormalSpeed = 0.0f;
    // As a door does: a void-out now restarts Link here.
    dComIfGs_setRestartRoom(dest, d.angleY, d.roomNo);
    if (camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0))) {
        camera->mCamera.Reset(camera->mCamera.mCenter + delta, camera->mCamera.mEye + delta);
    }
    interp::requestPresentationSync();
    return true;
}

u32 stageTeleportRoomParam(const Destination& d) {
    return daPy_py_c::setParamData(d.roomNo, 0, kStartEventDefaultStart, 0);
}

void startStageTeleport(const Destination& d, int8_t layerArg) {
    const cXyz pos(d.x, d.y, d.z);
    dComIfGs_setRestartRoom(pos, d.angleY, d.roomNo);
    dComIfGs_setRestartRoomParam(stageTeleportRoomParam(d));
    // setPoint 1 stores start point -1, so nothing matches a point of the stage we leave.
    dComIfGp_setNextStage(d.stageName, -1, d.roomNo, layerArg, 0.0f, 0, 1, 0, 0, 1, 0);
}

// A void-out before phase_1 takes the request would load the old stage here: take it back.
void keepStageTeleport(const Destination& d, int8_t layerArg) {
    if (dComIfGp_getNextStagePoint() != -1) {
        if (dComIfGs_getRestartRoomParam() != 0) {
            TwiliLog.info("[teleport] load replaced by an exit to {} point {}",
                dComIfGp_getNextStageName(), dComIfGp_getNextStagePoint());
            dComIfGs_setRestartRoomParam(0);
        }
        return;
    }
    const bool ours =
        std::strncmp(dComIfGp_getNextStageName(), d.stageName, sizeof(d.stageName)) == 0 &&
        dComIfGp_getNextStageRoomNo() == d.roomNo && dComIfGp_getNextStageLayer() == layerArg &&
        daPy_py_c::getLastSceneMode() == 0 &&
        dComIfGs_getRestartRoomParam() == stageTeleportRoomParam(d);
    if (!ours) {
        TwiliLog.warn("[teleport] load replaced by a restart in {} (mode {}); restoring it",
            dComIfGp_getNextStageName(), daPy_py_c::getLastSceneMode());
        startStageTeleport(d, layerArg);
    }
}

void finish(Result result, std::string reason, std::string message, bool cooldown = true) {
    Status& st = s_state.status;
    TwiliLog.info("[teleport] to client {} ended: {} {} - {}", st.targetClientId,
        resultName(result), reason, message);
    if (result != Result::MovedLocally && result != Result::ChangedStage) {
        ui::toast("Teleport", message, ui::kToastNet, 4000);
    }
    st.phase = Phase::Idle;
    st.lastResult = result;
    st.reason = std::move(reason);
    st.message = std::move(message);
    st.resultSeq++;
    s_state.requestId = 0;
    if (cooldown) {
        s_state.cooldownUntil = Clock::now() + kCooldown;
    }
}

void tickSafeSpot() {
    daAlink_c* link = liveLink();
    const char* stage = dComIfGp_getStartStageName();
    if (!session().isConnected() || !isSaveLoaded() || session().currentSaveTblNo() < 0 ||
        dComIfGp_isEnableNextStage() || link == nullptr || stage == nullptr || stage[0] == '\0')
    {
        s_safeSpot.valid = false;
        return;
    }
    const int8_t layerNo = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    Destination& d = s_safeSpot.dest;
    if (s_safeSpot.valid &&
        (d.layerNo != layerNo || std::strncmp(d.stageName, stage, sizeof(d.stageName)) != 0))
    {
        s_safeSpot.valid = false;
    }
    // A requester must be able to spawn here: safe floor, no event, not riding, not in water.
    if (dComIfGp_event_runCheck() || !onSafeFloor(link) || link->mLinkAcch.ChkWaterIn() ||
        link->checkRide() || link->mMode != 0 || link->mProcID == daAlink_c::PROC_DEAD)
    {
        return;
    }
    dBgS& bg = dComIfG_Bgsp();
    const cBgS_PolyInfo& gnd = link->mLinkAcch.m_gnd;
    // GetRoomId first: -1 for a stale polygon, which GetExitId would index with.
    const int roomNo = bg.GetRoomId(gnd);
    if (roomNo < 0 || roomNo >= 64 || bg.ChkMoveBG_NoDABg(gnd) || bg.GetExitId(gnd) != 0x3F ||
        !isTeleportableRoom(stage, roomNo))
    {
        return;
    }
    std::memset(d.stageName, 0, sizeof(d.stageName));
    std::strncpy(d.stageName, stage, sizeof(d.stageName) - 1);
    d.roomNo = static_cast<int8_t>(roomNo);
    d.layerNo = layerNo;
    d.x = link->current.pos.x;
    d.y = link->current.pos.y;
    d.z = link->current.pos.z;
    d.angleY = link->shape_angle.y;
    s_safeSpot.valid = true;
    s_safeSpot.capturedAt = Clock::now();
}

void handleRequestTeleport(const nlohmann::json& packet) {
    Session& s = session();
    const uint32_t self = s.selfClientId();
    const uint32_t from = packet.value("clientId", 0u);
    const uint32_t requestId = packet.value("requestId", 0u);
    if (packet.value("targetClientId", 0u) != self || from == 0 || from == self) {
        return;
    }
    nlohmann::json reply = {
        {"type", "TELEPORT_TO"},
        {"targetClientId", from},
        {"requestId", requestId},
        {"ok", false},
    };
    const auto refuse = [&](const char* code) {
        reply["reason"] = code;
        s.send(reply);
        TwiliLog.info("[teleport] refused request {} from client {} ({})", requestId, from, code);
    };

    State& t = s_state;
    if (!s.roomState().teleportMode) {
        return refuse("disabled");
    }
    if (!s.clients().count(from)) {
        return refuse("offline");
    }
    // Both asked for each other: the lower client id stays and answers, the other moves.
    const bool gaveWay =
        t.status.phase == Phase::AwaitingAnswer && t.status.targetClientId == from && self < from;
    if (t.status.phase != Phase::Idle && !gaveWay) {
        return refuse("busy");
    }
    if (!isSaveLoaded() || s.currentSaveTblNo() < 0 || dComIfGp_isEnableNextStage() ||
        liveLink() == nullptr)
    {
        return refuse("loading");
    }
    if (dComIfGp_event_runCheck() && dComIfGp_event_getMode() != dEvt_mode_TALK_e) {
        return refuse("cutscene");
    }
    if (!s_safeSpot.valid) {
        return refuse("no-safe-spot");
    }
    // Only now does our own request give way; after a refusal above both fail as busy.
    if (gaveWay) {
        finish(Result::Refused, "busy",
            fmt::format("{} is coming to you instead.", clientName(from)), false);
    }

    const Destination& d = s_safeSpot.dest;
    reply["ok"] = true;
    reply["stageName"] = std::string(d.stageName);
    reply["roomNo"] = static_cast<int>(d.roomNo);
    reply["layerNo"] = static_cast<int>(d.layerNo);
    reply["pos"] = {{"x", d.x}, {"y", d.y}, {"z", d.z}};
    reply["angleY"] = static_cast<int>(d.angleY);
    reply["ageMs"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_safeSpot.capturedAt)
            .count());
    s.send(reply);
    TwiliLog.info("[teleport] answered request {} from client {} with {} room {} layer {}",
        requestId, from, d.stageName, d.roomNo, d.layerNo);
    if (!gaveWay) {
        ui::toast("Teleport", fmt::format("{} is teleporting to you.", clientName(from)),
            ui::kToastNet, 3000);
    }
}

void handleTeleportTo(const nlohmann::json& packet) {
    State& t = s_state;
    const uint32_t from = packet.value("clientId", 0u);
    if (packet.value("targetClientId", 0u) != session().selfClientId()) {
        return;
    }
    if (t.status.phase != Phase::AwaitingAnswer || from != t.status.targetClientId ||
        packet.value("requestId", 0u) != t.requestId)
    {
        return;  // a request we already gave up on
    }
    const std::string name = clientName(from);
    if (!packet.value("ok", false)) {
        const std::string reason = packet.value("reason", std::string("refused"));
        finish(Result::Refused, reason, fmt::format("{}: {}.", name, reasonText(reason)));
        return;
    }

    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    const std::string stage = packet.value("stageName", std::string{});
    const int roomNo = packet.value("roomNo", -1);
    const int layerNo = packet.value("layerNo", -1);
    Destination d;
    std::strncpy(d.stageName, stage.c_str(), sizeof(d.stageName) - 1);
    d.roomNo = static_cast<int8_t>(roomNo >= 0 && roomNo < 64 ? roomNo : -1);
    d.layerNo = static_cast<int8_t>(layerNo >= 0 && layerNo < 15 ? layerNo : -1);
    d.angleY = static_cast<int16_t>(packet.value("angleY", 0));
    d.x = d.y = d.z = kNaN;
    if (const auto pos = packet.find("pos"); pos != packet.end() && pos->is_object()) {
        d.x = pos->value("x", kNaN);
        d.y = pos->value("y", kNaN);
        d.z = pos->value("z", kNaN);
    }
    if (stage.size() > 7 || !validDestination(d)) {
        finish(Result::Failed, "bad-destination",
            fmt::format("{} sent a destination teleporting cannot reach.", name));
        return;
    }
    // Decided again right before a reload (a door taken meanwhile changes our layer).
    if (!destinationLayerArg(d)) {
        finish(Result::Refused, "layer", layerRefusalMessage(name, d));
        return;
    }

    t.dest = d;
    t.executeDeadline = Clock::now() + kExecuteWindow;
    t.status.phase = Phase::Executing;
    t.status.message = fmt::format("Teleporting to {}...", name);
    TwiliLog.info("[teleport] destination {} room {} layer {} ({:.0f}, {:.0f}, {:.0f}), spot {} "
                  "ms old",
        d.stageName, d.roomNo, d.layerNo, d.x, d.y, d.z, packet.value("ageMs", 0));
}

void tickExecuting(const std::string& name) {
    State& t = s_state;
    const auto now = Clock::now();
    if (!session().roomState().teleportMode) {
        finish(Result::Refused, "disabled", "Teleporting was turned off by the room owner.");
        return;
    }
    const char* why = localTeleportBlocker();
    if (why == nullptr) {
        if (tryLocalTeleport(t.dest)) {
            finish(Result::MovedLocally, "", fmt::format("Teleported to {}.", name));
            return;
        }
        // Through the reload's fade Link could void out or burn from the air or a hazard floor.
        if (!onSafeFloor(liveLink())) {
            why = "footing";
        }
    }
    if (why != nullptr) {
        if (now > t.executeDeadline) {
            finish(Result::Failed, why,
                fmt::format("Teleport cancelled: you were {}.", reasonText(why)));
        }
        return;
    }
    const std::optional<int8_t> layerArg = destinationLayerArg(t.dest);
    if (!layerArg) {
        finish(Result::Refused, "layer", layerRefusalMessage(name, t.dest));
        return;
    }
    t.layerArg = *layerArg;
    startStageTeleport(t.dest, t.layerArg);
    t.status.phase = Phase::AwaitingArrival;
    t.sawStageUnload = false;
    t.leftOldStage = false;
    t.stageRequestedAt = now;
}

void tickArrival(const std::string& name) {
    State& t = s_state;
    const int saveTblNo = session().currentSaveTblNo();
    const bool pending = dComIfGp_isEnableNextStage();
    if (pending && !t.leftOldStage) {
        keepStageTeleport(t.dest, t.layerArg);
    }
    // The old Link and save table are gone before the new stage clears the request.
    if (pending || saveTblNo < 0) {
        t.sawStageUnload = true;
    }
    if (t.sawStageUnload && !pending) {
        t.leftOldStage = true;
    }
    daAlink_c* link = liveLink();
    if (t.leftOldStage && !pending && saveTblNo >= 0 && link != nullptr) {
        const char* stage = dComIfGp_getStartStageName();
        if (stage == nullptr ||
            std::strncmp(stage, t.dest.stageName, sizeof(t.dest.stageName)) != 0)
        {
            finish(Result::Failed, "diverted",
                fmt::format("Teleport ended in {} instead of {}.", stage != nullptr ? stage : "?",
                    t.dest.stageName));
            return;
        }
        const float dx = link->current.pos.x - t.dest.x;
        const float dz = link->current.pos.z - t.dest.z;
        TwiliLog.info(
            "[teleport] arrived {:.0f} units from the destination", std::sqrt(dx * dx + dz * dz));
        finish(Result::ChangedStage, "", fmt::format("Teleported to {}.", name));
    } else if (Clock::now() - t.stageRequestedAt > kArrivalTimeout) {
        TwiliLog.error("[teleport] {} room {} did not finish loading after {}s", t.dest.stageName,
            t.dest.roomNo,
            std::chrono::duration_cast<std::chrono::seconds>(kArrivalTimeout).count());
        finish(Result::Failed, "stuck", "The destination did not finish loading.");
    }
}

}  // namespace

const char* resultName(Result result) {
    switch (result) {
    case Result::MovedLocally:
        return "local";
    case Result::ChangedStage:
        return "stage";
    case Result::Refused:
        return "refused";
    case Result::TimedOut:
        return "timeout";
    case Result::Failed:
        return "failed";
    default:
        return "none";
    }
}

std::string reasonText(std::string_view code) {
    static constexpr struct {
        std::string_view code;
        const char* text;
    } kTexts[] = {
        {"not-connected", "not connected"},
        {"disabled", "teleporting is off in this room"},
        {"offline", "not connected"},
        {"not-in-game", "not in game"},
        {"pending", "teleport in progress"},
        {"cooldown", "just teleported"},
        {"loading", "loading"},
        {"menu", "in a menu"},
        {"cutscene", "in a cutscene"},
        {"down", "knocked out"},
        {"riding", "riding"},
        {"busy", "busy"},
        {"carrying", "carrying something"},
        {"footing", "not standing on solid ground"},
        {"no-safe-spot", "has not stood on solid ground here yet"},
        {"layer", "in a version of the area your progress does not load"},
        {"bad-destination", "sent an unusable destination"},
        {"rate-limited", "too many requests, try again"},
        {"other-team", "on another team"},
        {"other-game", "playing a different game"},
        {"timeout", "did not answer"},
        {"disconnected", "disconnected"},
        {"stuck", "the destination did not finish loading"},
        {"diverted", "ended up somewhere else"},
        // Story follow, catch-up and pull-in.
        {"nothing", "nothing to catch up on"},
        {"flags", "waiting for your team's story flags"},
        {"form", "arrived in the wrong form"},
        {"airborne", "in the air"},
        {"swimming", "swimming"},
        {"room", "in another room"},
        {"seen", "already seen"},
        {"trigger", "its trigger is not active for you"},
        {"different-event", "your area differs"},
        {"refused", "the game did not start it"},
        {"ended", "it ended first"},
    };
    for (const auto& e : kTexts) {
        if (e.code == code) {
            return e.text;
        }
    }
    return std::string(code);
}

std::string clientName(uint32_t clientId) {
    const auto& clients = session().clients();
    const auto it = clients.find(clientId);
    if (it == clients.end() || it->second.name.empty()) {
        return fmt::format("#{}", clientId);
    }
    return it->second.name;
}

const char* blockCode(uint32_t clientId) {
    const Session& s = session();
    if (!s.isConnected() || s.selfClientId() == 0) {
        return "not-connected";
    }
    if (!s.roomState().teleportMode) {
        return "disabled";
    }
    const auto it = s.clients().find(clientId);
    if (it == s.clients().end() || it->second.self || !it->second.online) {
        return "offline";
    }
    if (!it->second.isSaveLoaded || it->second.stageName[0] == '\0') {
        return "not-in-game";
    }
    if (const char* code = team_game::teleportBlock(clientId)) {
        return code;
    }
    if (s_state.status.phase != Phase::Idle || story::loadActive()) {
        return "pending";
    }
    if (Clock::now() < s_state.cooldownUntil) {
        return "cooldown";
    }
    return localTeleportBlocker();
}

bool request(uint32_t clientId) {
    State& t = s_state;
    if (const char* code = blockCode(clientId)) {
        if (t.status.phase == Phase::Idle) {
            t.status.targetClientId = clientId;
            finish(Result::Refused, code,
                fmt::format("Cannot teleport to {}: {}.", clientName(clientId), reasonText(code)),
                false);
        }
        return false;
    }
    t.requestId = t.nextRequestId++;
    t.sessionClientId = session().selfClientId();
    t.sentAt = Clock::now();
    t.status.phase = Phase::AwaitingAnswer;
    t.status.targetClientId = clientId;
    t.status.reason.clear();
    t.status.message = fmt::format("Asking {}...", clientName(clientId));
    session().send({
        {"type", "REQUEST_TELEPORT"},
        {"targetClientId", clientId},
        {"requestId", t.requestId},
    });
    TwiliLog.info("[teleport] request {} to client {}", t.requestId, clientId);
    return true;
}

const std::string& statusMessage() {
    return s_state.status.message;
}

const Status& status() {
    return s_state.status;
}

bool idle() {
    return s_state.status.phase == Phase::Idle;
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type == "REQUEST_TELEPORT") {
        handleRequestTeleport(packet);
    } else if (type == "TELEPORT_TO") {
        handleTeleportTo(packet);
    } else {
        return false;
    }
    return true;
}

void tick() {
    tickSafeSpot();

    State& t = s_state;
    if (t.status.phase == Phase::Idle) {
        return;
    }
    const Session& s = session();
    const std::string name = clientName(t.status.targetClientId);
    // A session reset changes our client id; a stage change already started is seen through.
    if (t.status.phase != Phase::AwaitingArrival &&
        (!s.isConnected() || s.selfClientId() != t.sessionClientId))
    {
        finish(Result::Failed, "disconnected", "Teleport cancelled: disconnected.", false);
        return;
    }

    switch (t.status.phase) {
    case Phase::Idle:
        return;
    case Phase::AwaitingAnswer:
        if (!s.clients().count(t.status.targetClientId)) {
            finish(Result::Refused, "offline", fmt::format("{} left.", name));
        } else if (Clock::now() - t.sentAt > kAnswerTimeout) {
            finish(Result::TimedOut, "timeout",
                fmt::format("{} did not answer (their game may not support teleporting).", name));
        }
        return;
    case Phase::Executing:
        tickExecuting(name);
        return;
    case Phase::AwaitingArrival:
        tickArrival(name);
        return;
    }
}

}  // namespace twili::teleport
