// Asks the target through the server; Link lands beside the target's latest pose, after a stage load
// when the target's room is not loaded here.

#include "teleport/Teleport.hpp"

#include "core/Host.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "game/TeamGame.hpp"
#include "story/Story.hpp"
#include "ui/Toasts.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_bg_s_roof_chk.h"
#include "d/d_bg_s_wtr_chk.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "d/d_kankyo.h"
#include "d/d_stage.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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
// After a stage load, how long Link may take to be free before he stays where he spawned.
constexpr auto kPlaceWindow = std::chrono::seconds(5);
constexpr auto kCooldown = std::chrono::seconds(2);
constexpr float kMaxCoord = 1.0e6f;
// A floor this far above a probed spot (a slope, a step) or kMaxDrop below it (a rider, a jump)
// counts as the floor under it.
constexpr float kFloorTolerance = 60.0f;
constexpr float kMaxDrop = 400.0f;
// Beside the target, clear of its dummy.
constexpr float kSideOffset = 80.0f;
constexpr float kHeadroom = 130.0f;
// The line from the target to a spot beside it runs at this height, above steps.
constexpr float kWaist = 80.0f;
// Water deeper than this over a floor is not searched for; Link swims long before.
constexpr float kWaterProbe = 500.0f;
// No floor at all this far under the target: its room is not loaded here.
constexpr float kLoadedProbeDepth = 5000.0f;
// DEFAULT_START: map event 0 would play the destination's story cutscene (South Faron).
constexpr int kStartEventDefaultStart = 0xCA;

struct Destination {
    char stageName[8] = {};
    int8_t roomNo = -1;
    int8_t layerNo = -1;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int16_t angleY = 0;
};

// Where the target's Link stands and faces.
struct Pose {
    bool valid = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int16_t angleY = 0;
};

// A floor Link can stand on.
struct Spot {
    cXyz pos;
    int roomNo = -1;
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
    // Where a stage load puts Link, and the fallback beside a target we cannot stand next to.
    Destination dest;
    // The target's pose when it answered.
    Pose exact;
    int8_t layerArg = -1;
    bool sawStageUnload = false;
    bool leftOldStage = false;
    Clock::time_point sentAt{}, executeDeadline{}, stageRequestedAt{}, placeDeadline{}, cooldownUntil{};
};

State s_state;
SafeSpot s_safeSpot;
float s_testOffset[3] = {};

Session& session() {
    return Session::instance();
}

bool finiteCoords(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) < kMaxCoord &&
           std::fabs(y) < kMaxCoord && std::fabs(z) < kMaxCoord;
}

bool validDestination(const Destination& d) {
    return d.stageName[0] != '\0' && d.roomNo >= 0 && d.roomNo < 64 && d.layerNo >= 0 &&
           d.layerNo < 15 && finiteCoords(d.x, d.y, d.z) && isTeleportableRoom(d.stageName, d.roomNo);
}

bool inStage(const Destination& d) {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && std::strncmp(d.stageName, stage, sizeof(d.stageName)) == 0;
}

// On their layer, or on the one our progress loads there: a cutscene layer of theirs never loads.
bool inDestinationScene(const Destination& d) {
    const int layer = dComIfG_play_c::getLayerNo(0);
    return inStage(d) &&
           (layer == d.layerNo ||
               layer == dComIfG_play_c::getLayerNo_common(d.stageName, d.roomNo, -1));
}

// -1 loads the layer our progress picks; only a reload of the layer we already run keeps it.
int8_t destinationLayerArg(const Destination& d) {
    const int layer = dComIfG_play_c::getLayerNo(0);
    return inStage(d) && layer == d.layerNo &&
                   dComIfG_play_c::getLayerNo_common(d.stageName, d.roomNo, -1) != layer ?
               d.layerNo :
               -1;
}

bool swimming(daAlink_c* link) {
    return link != nullptr && link->checkModeFlg(daAlink_c::MODE_SWIMMING);
}

float swimStartHeight(bool wolf) {
    return wolf ? daAlinkHIO_wlSwim_c0::m.mStartHeight : daAlinkHIO_swim_c0::m.mStartHeight;
}

// The floor under `at` if Link can stand there: a loaded room's fixed floor with no exit, void,
// damage or sinking sand, headroom above it, and no lava or water deep enough to swim.
std::optional<Spot> standableAt(const cXyz& at, bool wolf) {
    dBgS& bg = dComIfG_Bgsp();
    dBgS_LinkGndChk gnd;
    cXyz probe(at.x, at.y + kFloorTolerance, at.z);
    gnd.SetPos(&probe);
    const f32 floorY = bg.GroundCross(&gnd);
    if (floorY == -G_CM3D_F_INF || floorY < at.y - kMaxDrop) {
        return std::nullopt;
    }
    // GetRoomId first: -1 for a stale polygon, which the other getters would index with.
    const int roomNo = bg.GetRoomId(gnd);
    if (roomNo < 0 || roomNo >= 64 || bg.ChkMoveBG_NoDABg(gnd) || bg.GetExitId(gnd) != 0x3F) {
        return std::nullopt;
    }
    // Codes 4 and 10 void out, 5 drops into an exit, 9 is deadly fog; argument bit 0x80 restarts.
    const int code = bg.GetGroundCode(gnd);
    const int att1 = bg.GetPolyAtt1(gnd);
    if (code == 4 || code == 5 || code == 9 || code == 10 ||
        (dKy_pol_argument_get(&gnd) & 0x80) != 0 ||
        (bg.GetPolyAtt0(gnd) == 3 && att1 >= 1 && att1 <= 3))
    {
        return std::nullopt;
    }
    cXyz floor(at.x, floorY, at.z);
    dBgS_LinkRoofChk roof;
    roof.SetPos(floor);
    if (bg.RoofChk(&roof) - floorY < kHeadroom) {
        return std::nullopt;
    }
    dBgS_WtrChk water;
    water.Set(floor, floorY + kWaterProbe);
    if (bg.WaterChk(&water) &&
        (bg.GetPolyAtt0(water) == 6 || water.GetHeight() - floorY > swimStartHeight(wolf)))
    {
        return std::nullopt;
    }
    return Spot{floor, roomNo};
}

bool lineBlocked(const cXyz& from, const cXyz& to) {
    dBgS_LinkLinChk line;
    const cXyz a(from.x, from.y + kWaist, from.z);
    const cXyz b(to.x, from.y + kWaist, to.z);
    line.Set(&a, &b, liveLink());
    return dComIfG_Bgsp().LineCross(&line);
}

// To their right, left, behind or in front, reachable from them; else on their spot.
std::optional<Spot> spotBeside(const Pose& p, bool wolf) {
    const cXyz at(p.x, p.y, p.z);
    for (const int side : {0x4000, -0x4000, 0x8000, 0}) {
        const s16 dir = static_cast<s16>(p.angleY + side);
        const cXyz c(p.x + cM_ssin(dir) * kSideOffset, p.y, p.z + cM_scos(dir) * kSideOffset);
        if (!lineBlocked(at, c)) {
            if (std::optional<Spot> spot = standableAt(c, wolf)) {
                return spot;
            }
        }
    }
    return standableAt(at, wolf);
}

bool floorLoadedUnder(const Pose& p) {
    dBgS_LinkGndChk gnd;
    cXyz probe(p.x, p.y + kFloorTolerance, p.z);
    gnd.SetPos(&probe);
    const f32 floorY = dComIfG_Bgsp().GroundCross(&gnd);
    return floorY != -G_CM3D_F_INF && floorY > p.y - kLoadedProbeDepth;
}

// A fresh PLAYER_UPDATE while they share our stage and layer, else their pose from the answer.
Pose targetPose() {
    const State& t = s_state;
    Pose p = t.exact;
    if (!p.valid) {
        p = {true, t.dest.x, t.dest.y, t.dest.z, t.dest.angleY};
    }
    const Session& s = session();
    const auto it = s.clients().find(t.status.targetClientId);
    if (it != s.clients().end() && it->second.online && it->second.hasPlayerUpdate &&
        s.clientIsInCurrentLayer(it->second))
    {
        const Client& c = it->second;
        p = {true, c.posX, c.posY, c.posZ, c.shapeAngleY};
    }
    p.x += s_testOffset[0];
    p.y += s_testOffset[1];
    p.z += s_testOffset[2];
    return p;
}

// Behind Link at the distance and height the camera kept from him.
void resetCameraBehind(daAlink_c* link, const cXyz& oldLinkPos) {
    camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
    if (camera == nullptr) {
        return;
    }
    dCamera_c& cam = camera->mCamera;
    const cXyz toEye = cam.mEye - cam.mCenter;
    const float dist =
        std::clamp(std::sqrt(toEye.x * toEye.x + toEye.z * toEye.z), 250.0f, 600.0f);
    const float rise = std::clamp(toEye.y, 0.0f, 300.0f);
    const float lift = std::clamp(cam.mCenter.y - oldLinkPos.y, 50.0f, 200.0f);
    const s16 dir = link->shape_angle.y;
    const cXyz& pos = link->current.pos;
    const cXyz center(pos.x, pos.y + lift, pos.z);
    const cXyz eye(center.x - cM_ssin(dir) * dist, center.y + rise, center.z - cM_scos(dir) * dist);
    cam.Reset(center, eye);
}

void moveLink(daAlink_c* link, const Spot& spot, int16_t angleY) {
    const cXyz from = link->current.pos;
    // Forced: outside an event the call does nothing otherwise.
    link->setPlayerPosAndAngle(&spot.pos, angleY, TRUE);
    link->speedF = 0.0f;
    link->mNormalSpeed = 0.0f;
    // As a door does: a void-out now restarts Link here.
    dComIfGs_setRestartRoom(spot.pos, angleY, static_cast<s8>(spot.roomNo));
    resetCameraBehind(link, from);
    interp::requestPresentationSync();
}

bool placeBeside(daAlink_c* link) {
    const Pose pose = targetPose();
    const std::optional<Spot> spot = spotBeside(pose, link->checkWolf());
    if (!spot) {
        return false;
    }
    moveLink(link, *spot, pose.angleY);
    const float dx = spot->pos.x - pose.x;
    const float dz = spot->pos.z - pose.z;
    TwiliLog.info("[teleport] placed {:.0f} units from the target in room {}",
        std::sqrt(dx * dx + dz * dz), spot->roomNo);
    return true;
}

bool placeOnSpot(daAlink_c* link) {
    const Destination& d = s_state.dest;
    const std::optional<Spot> spot = standableAt(cXyz(d.x, d.y, d.z), link->checkWolf());
    if (!spot) {
        return false;
    }
    moveLink(link, *spot, d.angleY);
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
    const local::OwnStageRequest own(d.stageName, -1, d.roomNo, layerArg);
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
    st.placement.clear();
    st.resultSeq++;
    s_state.requestId = 0;
    if (cooldown) {
        s_state.cooldownUntil = Clock::now() + kCooldown;
    }
}

// We made it: beside the target, or on the spot it sent when the ground beside it would not do.
void arrived(Result result, bool exact, const std::string& name) {
    finish(result, "", fmt::format("Teleported to {}.", name));
    s_state.status.placement = exact ? "exact" : "fallback";
    if (!exact) {
        ui::toast("Teleport",
            fmt::format("Teleported near {}: there is no safe ground right beside them.", name),
            ui::kToastNet, 4000);
    }
}

// Why the target or the room rules teleporting to them out, or nullptr.
const char* targetBlockCode(uint32_t clientId) {
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
    return team_game::teleportBlock(clientId);
}

// Why our own state rules it out, or nullptr. The target's cutscene does not.
const char* selfBlockCode() {
    if (s_state.status.phase != Phase::Idle || story::loadActive()) {
        return "pending";
    }
    if (Clock::now() < s_state.cooldownUntil) {
        return "cooldown";
    }
    if (const char* why = localTeleportBlocker()) {
        return why;
    }
    return swimming(liveLink()) ? "swimming" : nullptr;
}

// Our own state reads "you are riding", so it is not taken for the target's.
std::string blockText(const char* code, bool self) {
    if (!self) {
        return reasonText(code);
    }
    if (std::strcmp(code, "pending") == 0) {
        return "a teleport is in progress";
    }
    if (std::strcmp(code, "cooldown") == 0) {
        return "you just teleported";
    }
    if (std::strcmp(code, "loading") == 0) {
        return "your game is loading";
    }
    return "you are " + reasonText(code);
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

// The standable start point (PLYR) of our room nearest to us.
std::optional<Destination> nearestStartPoint(daAlink_c* link, int roomNo) {
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr || roomNo < 0 || roomNo >= 64 || !isTeleportableRoom(stage, roomNo)) {
        return std::nullopt;
    }
    dStage_roomDt_c* dt = dComIfGp_roomControl_getStatusRoomDt(roomNo);
    stage_actor_class* plyr = dt != nullptr ? dt->getPlayer() : nullptr;
    if (plyr == nullptr) {
        return std::nullopt;
    }
    const stage_actor_data_class* entries = plyr->m_entries;
    std::optional<Destination> best;
    float bestDist = std::numeric_limits<float>::max();
    for (int i = 0; i < dt->getPlayerNum(); i++) {
        const cXyz pos = entries[i].base.position;
        const float dist = pos.abs2(link->current.pos);
        if (dist >= bestDist) {
            continue;
        }
        const std::optional<Spot> spot = standableAt(pos, link->checkWolf());
        if (!spot || spot->roomNo != roomNo) {
            continue;
        }
        Destination d;
        std::strncpy(d.stageName, stage, sizeof(d.stageName) - 1);
        d.roomNo = static_cast<int8_t>(roomNo);
        d.layerNo = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
        d.x = spot->pos.x;
        d.y = spot->pos.y;
        d.z = spot->pos.z;
        d.angleY = entries[i].base.angle.y;
        best = d;
        bestDist = dist;
    }
    return best;
}

// Where a requester's stage load puts Link: our safe spot in our room, else the nearest standable
// start point of the room, else our safe spot elsewhere in the stage.
std::optional<Destination> answerSpot(daAlink_c* link, bool& safeSpot) {
    const int roomNo = dComIfGp_roomControl_getStayNo();
    safeSpot = s_safeSpot.valid;
    if (s_safeSpot.valid && s_safeSpot.dest.roomNo == roomNo) {
        return s_safeSpot.dest;
    }
    if (std::optional<Destination> start = nearestStartPoint(link, roomNo)) {
        safeSpot = false;
        return start;
    }
    return s_safeSpot.valid ? std::optional<Destination>(s_safeSpot.dest) : std::nullopt;
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
    // A cutscene is no reason: the requester lands beside us and it plays on for us alone.
    daAlink_c* link = liveLink();
    if (!isSaveLoaded() || s.currentSaveTblNo() < 0 || dComIfGp_isEnableNextStage() ||
        link == nullptr || link->mProcID == daAlink_c::PROC_WARP)
    {
        return refuse("loading");
    }
    bool safeSpot = false;
    const std::optional<Destination> spot = answerSpot(link, safeSpot);
    if (!spot) {
        return refuse("no-safe-spot");
    }
    // Only now does our own request give way; after a refusal above both fail as busy.
    if (gaveWay) {
        finish(Result::Refused, "busy",
            fmt::format("{} is coming to you instead.", clientName(from)), false);
    }

    const Destination& d = *spot;
    reply["ok"] = true;
    reply["stageName"] = std::string(d.stageName);
    reply["roomNo"] = static_cast<int>(d.roomNo);
    reply["layerNo"] = static_cast<int>(d.layerNo);
    reply["pos"] = {{"x", d.x}, {"y", d.y}, {"z", d.z}};
    reply["angleY"] = static_cast<int>(d.angleY);
    reply["ageMs"] = safeSpot ? static_cast<int64_t>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - s_safeSpot.capturedAt)
                                        .count()) :
                                int64_t{0};
    reply["at"] = {
        {"x", link->current.pos.x},
        {"y", link->current.pos.y},
        {"z", link->current.pos.z},
        {"angleY", static_cast<int>(link->shape_angle.y)},
    };
    s.send(reply);
    TwiliLog.info("[teleport] answered request {} from client {} with {} room {} layer {} ({})",
        requestId, from, d.stageName, d.roomNo, d.layerNo, safeSpot ? "safe spot" : "start point");
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
    Pose exact;
    if (const auto at = packet.find("at"); at != packet.end() && at->is_object()) {
        exact.x = at->value("x", kNaN);
        exact.y = at->value("y", kNaN);
        exact.z = at->value("z", kNaN);
        exact.angleY = static_cast<int16_t>(at->value("angleY", 0));
        exact.valid = finiteCoords(exact.x, exact.y, exact.z);
    }

    t.dest = d;
    t.exact = exact;
    t.executeDeadline = Clock::now() + kExecuteWindow;
    t.status.phase = Phase::Executing;
    t.status.message = fmt::format("Teleporting to {}...", name);
    TwiliLog.info("[teleport] destination {} room {} layer {} ({:.0f}, {:.0f}, {:.0f}), spot {} ms "
                  "old; target at ({:.0f}, {:.0f}, {:.0f})",
        d.stageName, d.roomNo, d.layerNo, d.x, d.y, d.z, packet.value("ageMs", 0), exact.x,
        exact.y, exact.z);
}

void tickExecuting(const std::string& name) {
    State& t = s_state;
    const auto now = Clock::now();
    if (!session().roomState().teleportMode) {
        finish(Result::Refused, "disabled", "Teleporting was turned off by the room owner.");
        return;
    }
    const char* why = localTeleportBlocker();
    if (why == nullptr && swimming(liveLink())) {
        why = "swimming";
    }
    if (why == nullptr) {
        daAlink_c* link = liveLink();
        if (inDestinationScene(t.dest) && isSettledOnGround(link)) {
            if (placeBeside(link)) {
                arrived(Result::MovedLocally, true, name);
                return;
            }
            // Their room is loaded here, but the ground beside them will not do.
            if (floorLoadedUnder(targetPose()) && placeOnSpot(link)) {
                arrived(Result::MovedLocally, false, name);
                return;
            }
        }
        // Through the reload's fade Link could void out or burn from the air or a hazard floor.
        if (!onSafeFloor(link)) {
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
    t.layerArg = destinationLayerArg(t.dest);
    startStageTeleport(t.dest, t.layerArg);
    t.status.phase = Phase::AwaitingArrival;
    t.sawStageUnload = false;
    t.leftOldStage = false;
    t.stageRequestedAt = now;
}

void tickArrival() {
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
        if (!inStage(t.dest)) {
            finish(Result::Failed, "diverted",
                fmt::format("Teleport ended in {} instead of {}.", stage != nullptr ? stage : "?",
                    t.dest.stageName));
            return;
        }
        const float dx = link->current.pos.x - t.dest.x;
        const float dz = link->current.pos.z - t.dest.z;
        TwiliLog.info(
            "[teleport] arrived {:.0f} units from the spot", std::sqrt(dx * dx + dz * dz));
        t.status.phase = Phase::Placing;
        t.placeDeadline = Clock::now() + kPlaceWindow;
    } else if (Clock::now() - t.stageRequestedAt > kArrivalTimeout) {
        TwiliLog.error("[teleport] {} room {} did not finish loading after {}s", t.dest.stageName,
            t.dest.roomNo,
            std::chrono::duration_cast<std::chrono::seconds>(kArrivalTimeout).count());
        finish(Result::Failed, "stuck", "The destination did not finish loading.");
    }
}

// Once Link is free in the new stage and the target's room has collision: beside the target.
void tickPlacing(const std::string& name) {
    daAlink_c* link = liveLink();
    if (dComIfGp_isEnableNextStage()) {
        arrived(Result::ChangedStage, false, name);
        return;
    }
    if (link != nullptr && localTeleportBlocker() == nullptr && !swimming(link) &&
        isSettledOnGround(link))
    {
        if (placeBeside(link)) {
            arrived(Result::ChangedStage, true, name);
            return;
        }
        if (floorLoadedUnder(targetPose())) {
            arrived(Result::ChangedStage, false, name);
            return;
        }
    }
    if (Clock::now() > s_state.placeDeadline) {
        arrived(Result::ChangedStage, false, name);
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
        {"no-safe-spot", "has no safe ground to send you to"},
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
    const char* code = targetBlockCode(clientId);
    return code != nullptr ? code : selfBlockCode();
}

Offer offer(uint32_t clientId) {
    const Session& s = session();
    const auto it = s.clients().find(clientId);
    if (!s.isConnected() || it == s.clients().end() || it->second.self || !it->second.online ||
        team_game::teleportBlock(clientId) != nullptr)
    {
        return Offer::None;
    }
    return blockCode(clientId) == nullptr ? Offer::Ready : Offer::Blocked;
}

std::string offerTooltip(uint32_t clientId) {
    const std::string name = clientName(clientId);
    const char* target = targetBlockCode(clientId);
    const char* self = target == nullptr ? selfBlockCode() : nullptr;
    if (target == nullptr && self == nullptr) {
        return "Teleport to " + name;
    }
    return fmt::format(
        "Teleport to {} ({})", name, blockText(target != nullptr ? target : self, self != nullptr));
}

bool request(uint32_t clientId) {
    State& t = s_state;
    const char* target = targetBlockCode(clientId);
    const char* self = target == nullptr ? selfBlockCode() : nullptr;
    if (const char* code = target != nullptr ? target : self) {
        if (t.status.phase == Phase::Idle) {
            t.status.targetClientId = clientId;
            finish(Result::Refused, code,
                fmt::format("Cannot teleport to {}: {}.", clientName(clientId),
                    blockText(code, self != nullptr)),
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
    t.status.placement.clear();
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
    if (t.status.phase != Phase::AwaitingArrival && t.status.phase != Phase::Placing &&
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
        tickArrival();
        return;
    case Phase::Placing:
        tickPlacing(name);
        return;
    }
}

void setPlacementOffsetForTest(float dx, float dy, float dz) {
    s_testOffset[0] = dx;
    s_testOffset[1] = dy;
    s_testOffset[2] = dz;
}

}  // namespace twili::teleport
