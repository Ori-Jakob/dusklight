#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace twili {

// Room-wide settings; only the owner may change them.
struct RoomState {
    uint32_t ownerClientId = 0;
    bool pvpMode = false;
    bool pvpFriendlyFire = false;
    bool pvpLethal = false;
    bool showLocationsMode = true;
    bool teleportMode = false;
    // Teleport to other teams' players (never between two different team games).
    bool teleportAcrossTeams = false;
    bool syncWorldState = true;
    bool shareWoodenShield = true;
    // Enemy-death sync ("Sync Enemy Deaths").
    bool syncNPCs = false;
    // Needs syncNPCs ("Share Enemy Damage").
    bool syncEnemyDamage = true;
    bool cutsceneSync = true;
    bool hidePlayersInCutscene = false;
    int enemyHealthMultiplier = 100;  // percent

    // Compares everything except ownerClientId.
    bool sameSettings(const RoomState& o) const {
        return pvpMode == o.pvpMode && pvpFriendlyFire == o.pvpFriendlyFire &&
               pvpLethal == o.pvpLethal && showLocationsMode == o.showLocationsMode &&
               teleportMode == o.teleportMode && teleportAcrossTeams == o.teleportAcrossTeams &&
               syncWorldState == o.syncWorldState && shareWoodenShield == o.shareWoodenShield &&
               syncNPCs == o.syncNPCs && syncEnemyDamage == o.syncEnemyDamage &&
               cutsceneSync == o.cutsceneSync &&
               hidePlayersInCutscene == o.hidePlayersInCutscene &&
               enemyHealthMultiplier == o.enemyHealthMultiplier;
    }

    nlohmann::json toJson() const {
        return {
            {"ownerClientId", ownerClientId},
            {"pvpMode", pvpMode},
            {"pvpFriendlyFire", pvpFriendlyFire},
            {"pvpLethal", pvpLethal},
            {"showLocationsMode", showLocationsMode},
            {"teleportMode", teleportMode},
            {"teleportAcrossTeams", teleportAcrossTeams},
            {"syncWorldState", syncWorldState},
            {"shareWoodenShield", shareWoodenShield},
            {"syncNPCs", syncNPCs},
            {"syncEnemyDamage", syncEnemyDamage},
            {"cutsceneSync", cutsceneSync},
            {"hidePlayersInCutscene", hidePlayersInCutscene},
            {"enemyHealthMultiplier", enemyHealthMultiplier},
        };
    }

    // Missing keys keep their value.
    void applyJson(const nlohmann::json& j) {
        if (!j.is_object()) return;
        ownerClientId = j.value("ownerClientId", ownerClientId);
        pvpMode = j.value("pvpMode", pvpMode);
        pvpFriendlyFire = j.value("pvpFriendlyFire", pvpFriendlyFire);
        pvpLethal = j.value("pvpLethal", pvpLethal);
        showLocationsMode = j.value("showLocationsMode", showLocationsMode);
        teleportMode = j.value("teleportMode", teleportMode);
        teleportAcrossTeams = j.value("teleportAcrossTeams", teleportAcrossTeams);
        syncWorldState = j.value("syncWorldState", syncWorldState);
        shareWoodenShield = j.value("shareWoodenShield", shareWoodenShield);
        syncNPCs = j.value("syncNPCs", syncNPCs);
        syncEnemyDamage = j.value("syncEnemyDamage", syncEnemyDamage);
        cutsceneSync = j.value("cutsceneSync", cutsceneSync);
        hidePlayersInCutscene = j.value("hidePlayersInCutscene", hidePlayersInCutscene);
        enemyHealthMultiplier = j.value("enemyHealthMultiplier", enemyHealthMultiplier);
    }
};

}  // namespace twili
