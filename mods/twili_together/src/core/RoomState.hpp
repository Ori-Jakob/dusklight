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
    bool syncWorldState = true;
    bool shareWoodenShield = true;
    // Enemy-death sync ("Sync Enemy Deaths").
    bool syncNPCs = false;
    bool cutsceneSync = true;
    bool hidePlayersInCutscene = false;
    int enemyCountMultiplier = 100;   // percent
    int enemyHealthMultiplier = 100;  // percent

    // Compares everything except ownerClientId.
    bool sameSettings(const RoomState& o) const {
        return pvpMode == o.pvpMode && pvpFriendlyFire == o.pvpFriendlyFire &&
               pvpLethal == o.pvpLethal && showLocationsMode == o.showLocationsMode &&
               teleportMode == o.teleportMode && syncWorldState == o.syncWorldState &&
               shareWoodenShield == o.shareWoodenShield &&
               syncNPCs == o.syncNPCs && cutsceneSync == o.cutsceneSync &&
               hidePlayersInCutscene == o.hidePlayersInCutscene &&
               enemyCountMultiplier == o.enemyCountMultiplier &&
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
            {"syncWorldState", syncWorldState},
            {"shareWoodenShield", shareWoodenShield},
            {"syncNPCs", syncNPCs},
            {"cutsceneSync", cutsceneSync},
            {"hidePlayersInCutscene", hidePlayersInCutscene},
            {"enemyCountMultiplier", enemyCountMultiplier},
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
        syncWorldState = j.value("syncWorldState", syncWorldState);
        shareWoodenShield = j.value("shareWoodenShield", shareWoodenShield);
        syncNPCs = j.value("syncNPCs", syncNPCs);
        cutsceneSync = j.value("cutsceneSync", cutsceneSync);
        hidePlayersInCutscene = j.value("hidePlayersInCutscene", hidePlayersInCutscene);
        enemyCountMultiplier = j.value("enemyCountMultiplier", enemyCountMultiplier);
        enemyHealthMultiplier = j.value("enemyHealthMultiplier", enemyHealthMultiplier);
    }
};

}  // namespace twili
