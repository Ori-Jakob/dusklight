// UPDATE_ROOM_STATE: the room owner's settings are the source of truth.

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include <nlohmann/json.hpp>

#include <chrono>

namespace twili {

static constexpr auto kRoomStatePushInterval = std::chrono::milliseconds(250);
// Pushed again when the echo never matched (a relay that drops unknown keys).
static constexpr auto kRoomStateRepushInterval = std::chrono::seconds(5);

RoomState Session::roomStateFromSettings() const {
    using config::Var;
    RoomState state;
    state.ownerClientId = mSelfClientId;
    state.pvpMode = config::getBool(Var::PvpMode);
    state.pvpFriendlyFire = config::getBool(Var::PvpFriendlyFire);
    state.pvpLethal = config::getBool(Var::PvpLethal);
    state.showLocationsMode = config::getBool(Var::ShowLocations);
    state.teleportMode = config::getBool(Var::TeleportMode);
    state.teleportAcrossTeams = config::getBool(Var::TeleportAcrossTeams);
    state.syncWorldState = config::getBool(Var::SyncWorldState);
    state.shareWoodenShield = config::getBool(Var::ShareWoodenShield);
    state.syncNPCs = config::getBool(Var::SyncEnemyDeaths);
    state.syncEnemyDamage = config::getBool(Var::SyncEnemyDamage);
    state.cutsceneSync = config::getBool(Var::CutsceneSync);
    state.hidePlayersInCutscene = config::getBool(Var::HidePlayersInCutscene);
    state.enemyHealthMultiplier = static_cast<int>(config::getInt(Var::EnemyHealthMultiplier));
    return state;
}

void Session::sendUpdateRoomState() {
    if (!isRoomOwner()) {
        return;
    }
    mLastPushedRoomState = roomStateFromSettings();
    send({
        {"type", "UPDATE_ROOM_STATE"},
        {"state", mLastPushedRoomState.toJson()},
    });
    mLastRoomStatePush = std::chrono::steady_clock::now();
}

void Session::tickRoomOwnerSettings() {
    if (!isRoomOwner()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastRoomStatePush < kRoomStatePushInterval) {
        return;
    }
    const RoomState local = roomStateFromSettings();
    if (!local.sameSettings(mRoomState) &&
        (!local.sameSettings(mLastPushedRoomState) ||
         now - mLastRoomStatePush >= kRoomStateRepushInterval))
    {
        sendUpdateRoomState();
    }
}

void Session::handleUpdateRoomState(const nlohmann::json& packet) {
    const auto state = packet.find("state");
    const nlohmann::json& j = (state != packet.end() && state->is_object()) ? *state : packet;

    const uint32_t previousOwner = mRoomState.ownerClientId;
    const bool wasSyncingWorld = mRoomState.syncWorldState;
    mRoomState.applyJson(j);
    if (mRoomState.ownerClientId != previousOwner) {
        TwiliLog.info("[session] room owner is now client {}{}", mRoomState.ownerClientId,
                     mRoomState.ownerClientId == mSelfClientId ? " (you)" : "");
    }
    if (!wasSyncingWorld && mRoomState.syncWorldState) {
        sync::requestExchange();
    }
}

}  // namespace twili
