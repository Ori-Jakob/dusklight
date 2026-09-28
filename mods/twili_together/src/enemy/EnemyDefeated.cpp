// ENEMY_DEFEATED: one update's kills in one stage and layer; relayed to teammates there, uncached.

#include "enemy/EnemySync.hpp"

#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace twili::enemy_sync {
namespace {

// Every field is checked for type and range; a kill with a bad field is skipped on its own.
bool intField(const nlohmann::json& j, const char* key, int64_t lo, int64_t hi, int64_t& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        return false;
    }
    out = it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX) ?
              INT64_MAX :
              it->get<int64_t>();
    return out >= lo && out <= hi;
}

bool coordField(const nlohmann::json& j, const char* key, float& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return false;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v) || std::fabs(v) >= 1e6) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

bool parseKill(const nlohmann::json& k, Kill& out) {
    if (!k.is_object()) {
        return false;
    }
    int64_t roomNo, procName, params, setId;
    if (!intField(k, "roomNo", -1, 63, roomNo) || !intField(k, "procName", 0, 0x7FFF, procName) ||
        !intField(k, "params", 0, 0xFFFFFFFFll, params) || !intField(k, "setId", 0, 0xFFFF, setId))
    {
        return false;
    }
    const auto home = k.find("home");
    if (home == k.end() || !home->is_object() || !coordField(*home, "x", out.key.home[0]) ||
        !coordField(*home, "y", out.key.home[1]) || !coordField(*home, "z", out.key.home[2]))
    {
        return false;
    }
    out.key.roomNo = static_cast<int8_t>(roomNo);
    out.key.procName = static_cast<int16_t>(procName);
    out.key.params = static_cast<uint32_t>(params);
    out.key.setId = static_cast<uint16_t>(setId);
    // Cosmetic: clamped rather than rejected.
    int64_t v;
    out.fxSize = intField(k, "fxSize", INT64_MIN, INT64_MAX, v) ?
                     static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255)) :
                     10;
    out.fxType = intField(k, "fxType", INT64_MIN, INT64_MAX, v) ?
                     static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 3)) :
                     0;
    const auto zone = k.find("zoneActor");
    out.zoneActor = zone != k.end() && zone->is_boolean() && zone->get<bool>();
    return true;
}

void handleEnemyDefeated(const nlohmann::json& packet) {
    const Session& session = Session::instance();
    if (!session.isConnected() || !session.roomState().syncNPCs || !isSaveLoaded()) {
        return;
    }
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == session.selfClientId() ||
        packet.value("senderSessionKey", std::string{}) == session.sessionKey() ||
        packet.value("teamId", std::string{}) != session.selfTeamId() ||
        packet.value("protocolVersion", -1) != Session::kProtocolVersion ||
        packet.value("v", 0) != kVersion)
    {
        return;
    }
    // Checked again against where we are now: we may have moved on meanwhile.
    const char* stage = dComIfGp_getStartStageName();
    const std::string pStage = packet.value("stageName", std::string{});
    if (stage == nullptr || pStage.size() > 7 || std::strncmp(stage, pStage.c_str(), 8) != 0 ||
        packet.value("layerNo", -128) != dComIfG_play_c::getLayerNo(0))
    {
        return;
    }
    const auto kills = packet.find("kills");
    if (kills == packet.end() || !kills->is_array() || kills->size() > kMaxKillsPerPacket) {
        return;
    }
    for (const auto& k : *kills) {
        Kill kill;
        if (parseKill(k, kill)) {
            detail::queueRemote(kill);
        }
    }
    // Whatever can be removed right away is, in the same update.
    tick();
}

}  // namespace

bool detail::sendDefeated(const char* stage, int layer, const Kill* kills, size_t count) {
    Session& session = Session::instance();
    // The server routes by the stage it last heard from us.
    if (count == 0 || std::strncmp(stage, session.reportedStageName(), 8) != 0 ||
        layer != session.reportedLayerNo())
    {
        return false;
    }
    const auto& clients = session.clients();
    const bool teammateHere = std::any_of(clients.begin(), clients.end(), [&](const auto& e) {
        return !e.second.self && session.clientIsInCurrentLayer(e.second) &&
               session.isTeammate(e.second);
    });
    if (!teammateHere) {
        return false;
    }
    nlohmann::json list = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        const Kill& k = kills[i];
        list.push_back({
            {"roomNo", static_cast<int>(k.key.roomNo)},
            {"procName", k.key.procName},
            {"params", k.key.params},
            {"setId", k.key.setId},
            {"home", {{"x", k.key.home[0]}, {"y", k.key.home[1]}, {"z", k.key.home[2]}}},
            {"fxSize", static_cast<int>(k.fxSize)},
            {"fxType", static_cast<int>(k.fxType)},
            {"zoneActor", k.zoneActor},
        });
    }
    nlohmann::json packet = {
        {"type", "ENEMY_DEFEATED"},
        {"v", kVersion},
        {"stageName", std::string(stage)},
        {"layerNo", layer},
        {"kills", std::move(list)},
    };
    sync::stampPacket(packet, false);
    session.send(packet);
    return true;
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type != "ENEMY_DEFEATED") {
        return false;
    }
    handleEnemyDefeated(packet);
    return true;
}

}  // namespace twili::enemy_sync
