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
    if (!detail::parseKey(k, out.key)) {
        return false;
    }
    // Cosmetic: clamped rather than rejected.
    int64_t v;
    out.fxSize = detail::intField(k, "fxSize", INT64_MIN, INT64_MAX, v) ?
                     static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255)) :
                     10;
    out.fxType = detail::intField(k, "fxType", INT64_MIN, INT64_MAX, v) ?
                     static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 3)) :
                     0;
    const auto zone = k.find("zoneActor");
    out.zoneActor = zone != k.end() && zone->is_boolean() && zone->get<bool>();
    return true;
}

void handleEnemyDefeated(const nlohmann::json& packet) {
    const Session& session = Session::instance();
    if (!session.isConnected() || !session.roomState().syncNPCs || !isSaveLoaded() ||
        !detail::acceptFromTeammate(packet, kVersion))
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

// Every field is checked for type and range; an entry with a bad field is skipped on its own.
bool detail::intField(
    const nlohmann::json& j, const char* key, int64_t lo, int64_t hi, int64_t& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        return false;
    }
    out = it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX) ?
              INT64_MAX :
              it->get<int64_t>();
    return out >= lo && out <= hi;
}

bool detail::parseKey(const nlohmann::json& k, SpawnKey& out) {
    if (!k.is_object()) {
        return false;
    }
    int64_t roomNo, procName, params, setId, dup;
    if (!intField(k, "roomNo", -1, 63, roomNo) || !intField(k, "procName", 0, 0x7FFF, procName) ||
        !intField(k, "params", 0, 0xFFFFFFFFll, params) || !intField(k, "setId", 0, 0xFFFF, setId) ||
        !intField(k, "dup", 0, kMaxDup, dup))
    {
        return false;
    }
    const auto home = k.find("home");
    if (home == k.end() || !home->is_object() || !coordField(*home, "x", out.home[0]) ||
        !coordField(*home, "y", out.home[1]) || !coordField(*home, "z", out.home[2]))
    {
        return false;
    }
    out.roomNo = static_cast<int8_t>(roomNo);
    out.procName = static_cast<int16_t>(procName);
    out.params = static_cast<uint32_t>(params);
    out.setId = static_cast<uint16_t>(setId);
    out.dup = static_cast<uint8_t>(dup);
    return true;
}

nlohmann::json detail::keyJson(const SpawnKey& k) {
    return {
        {"roomNo", static_cast<int>(k.roomNo)},
        {"procName", k.procName},
        {"params", k.params},
        {"setId", k.setId},
        {"dup", static_cast<int>(k.dup)},
        {"home", {{"x", k.home[0]}, {"y", k.home[1]}, {"z", k.home[2]}}},
    };
}

bool detail::acceptFromTeammate(const nlohmann::json& packet, int version) {
    const Session& session = Session::instance();
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == session.selfClientId() ||
        packet.value("senderSessionKey", std::string{}) == session.sessionKey() ||
        packet.value("teamId", std::string{}) != session.selfTeamId() ||
        packet.value("protocolVersion", -1) != Session::kProtocolVersion ||
        packet.value("v", 0) != version)
    {
        return false;
    }
    // Checked again against where we are now: we may have moved on meanwhile.
    const char* stage = dComIfGp_getStartStageName();
    const std::string pStage = packet.value("stageName", std::string{});
    return stage != nullptr && pStage.size() <= 7 && std::strncmp(stage, pStage.c_str(), 8) == 0 &&
           packet.value("layerNo", -128) == dComIfG_play_c::getLayerNo(0);
}

bool detail::canSendHere(const char* stage, int layer) {
    const Session& session = Session::instance();
    // The server routes by the stage it last heard from us.
    if (std::strncmp(stage, session.reportedStageName(), 8) != 0 ||
        layer != session.reportedLayerNo())
    {
        return false;
    }
    const auto& clients = session.clients();
    return std::any_of(clients.begin(), clients.end(), [&](const auto& e) {
        return !e.second.self && session.clientIsInCurrentLayer(e.second) &&
               session.isTeammate(e.second);
    });
}

bool detail::sendDefeated(const char* stage, int layer, const Kill* kills, size_t count) {
    if (count == 0 || !canSendHere(stage, layer)) {
        return false;
    }
    Session& session = Session::instance();
    nlohmann::json list = nlohmann::json::array();
    for (size_t i = 0; i < count; ++i) {
        const Kill& k = kills[i];
        nlohmann::json j = detail::keyJson(k.key);
        j["fxSize"] = static_cast<int>(k.fxSize);
        j["fxType"] = static_cast<int>(k.fxType);
        j["zoneActor"] = k.zoneActor;
        list.push_back(std::move(j));
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
