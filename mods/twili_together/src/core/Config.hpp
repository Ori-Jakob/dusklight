#pragma once

#include <mods/svc/config.h>

#include <cstdint>
#include <string>

// Keys are "mod.dev_n0ted_twili__together.<name>".
namespace twili::config {

enum class Var : uint8_t {
    ServerUrl,
    DisplayName,
    TeamId,
    RoomId,
    Color,
    PvpMode,
    PvpFriendlyFire,
    PvpLethal,
    ShowLocations,
    TeleportMode,
    SyncWorldState,
    ShareWoodenShield,
    SyncEnemyDeaths,
    CutsceneSync,
    HidePlayersInCutscene,
    EnemyCountMultiplier,
    EnemyHealthMultiplier,
    StoryPrompts,
    ItemToasts,
    ItemToastsOwn,
    AutoReconnect,
#if TWILI_ENABLE_AUTOTEST
    AutotestScript,
#endif
    Count,
};

ModResult registerAll();

const char* name(Var var);
ConfigVarHandle handle(Var var);

bool getBool(Var var);
int64_t getInt(Var var);
std::string getString(Var var);

ModResult setBool(Var var, bool value);
ModResult setInt(Var var, int64_t value);
ModResult setString(Var var, const std::string& value);

// Host setting via find_host_var, or `fallback` when unavailable.
bool hostBool(const char* key, bool fallback);

}  // namespace twili::config
