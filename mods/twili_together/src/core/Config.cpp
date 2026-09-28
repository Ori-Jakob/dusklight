#include "core/Config.hpp"

#include "core/Log.hpp"

#include <array>
#include <unordered_map>

namespace twili::config {
namespace {

struct VarDef {
    Var var;
    const char* name;
    ConfigVarType type;
    bool defaultBool = false;
    int64_t defaultInt = 0;
    const char* defaultString = "";
};

// Room settings: the room owner's values apply.
constexpr VarDef kVars[] = {
    {Var::ServerUrl, "server_url", CONFIG_VAR_STRING},
    {Var::DisplayName, "display_name", CONFIG_VAR_STRING},
    {Var::TeamId, "team_id", CONFIG_VAR_STRING},
    {Var::RoomId, "room_id", CONFIG_VAR_STRING},
    {Var::Color, "color", CONFIG_VAR_STRING, false, 0, "#FFFFFF"},
    {Var::PvpMode, "pvp_mode", CONFIG_VAR_BOOL},
    {Var::PvpFriendlyFire, "pvp_friendly_fire", CONFIG_VAR_BOOL},
    {Var::PvpLethal, "pvp_lethal", CONFIG_VAR_BOOL},
    {Var::ShowLocations, "show_locations", CONFIG_VAR_BOOL, true},
    {Var::TeleportMode, "teleport_mode", CONFIG_VAR_BOOL},
    {Var::SyncWorldState, "sync_world_state", CONFIG_VAR_BOOL, true},
    {Var::ShareWoodenShield, "share_wooden_shield", CONFIG_VAR_BOOL, true},
    {Var::SyncEnemyDeaths, "sync_enemy_deaths", CONFIG_VAR_BOOL},
    {Var::CutsceneSync, "cutscene_sync", CONFIG_VAR_BOOL, true},
    {Var::HidePlayersInCutscene, "hide_players_in_cutscene", CONFIG_VAR_BOOL},
    {Var::EnemyCountMultiplier, "enemy_count_multiplier", CONFIG_VAR_INT, false, 100},
    {Var::EnemyHealthMultiplier, "enemy_health_multiplier", CONFIG_VAR_INT, false, 100},
    {Var::StoryPrompts, "story_prompts", CONFIG_VAR_BOOL, true},
    {Var::ItemToasts, "item_toasts", CONFIG_VAR_BOOL, true},
    {Var::ItemToastsOwn, "item_toasts_own", CONFIG_VAR_BOOL},
    {Var::AutoReconnect, "auto_reconnect", CONFIG_VAR_BOOL, true},
#if TWILI_ENABLE_AUTOTEST
    {Var::AutotestScript, "autotest_script", CONFIG_VAR_STRING},
#endif
};
static_assert(std::size(kVars) == static_cast<size_t>(Var::Count), "every Var needs a VarDef");

std::array<ConfigVarHandle, static_cast<size_t>(Var::Count)> s_handles{};

const VarDef& def(Var var) {
    return kVars[static_cast<size_t>(var)];
}

}  // namespace

ModResult registerAll() {
    for (size_t i = 0; i < std::size(kVars); ++i) {
        const VarDef& var = kVars[i];
        if (static_cast<size_t>(var.var) != i) {
            TwiliLog.error("[config] var table out of order at {}", var.name);
            return MOD_ERROR;
        }
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = var.name;
        desc.type = var.type;
        desc.default_bool = var.defaultBool;
        desc.default_int = var.defaultInt;
        desc.default_string = var.defaultString;
        const ModResult result = svc_config->register_var(mod_ctx, &desc, &s_handles[i]);
        if (result != MOD_OK) {
            TwiliLog.error(
                "[config] registering '{}' failed ({})", var.name, static_cast<int>(result));
            return result;
        }
    }
    return MOD_OK;
}

const char* name(Var var) {
    return def(var).name;
}

ConfigVarHandle handle(Var var) {
    return s_handles[static_cast<size_t>(var)];
}

bool getBool(Var var) {
    bool value = def(var).defaultBool;
    svc_config->get_bool(mod_ctx, handle(var), &value);
    return value;
}

int64_t getInt(Var var) {
    int64_t value = def(var).defaultInt;
    svc_config->get_int(mod_ctx, handle(var), &value);
    return value;
}

std::string getString(Var var) {
    size_t length = 0;
    if (svc_config->get_string(mod_ctx, handle(var), nullptr, 0, &length) != MOD_OK) {
        return def(var).defaultString;
    }
    std::string value(length, '\0');
    if (svc_config->get_string(mod_ctx, handle(var), value.data(), length + 1, nullptr) != MOD_OK) {
        return def(var).defaultString;
    }
    return value;
}

ModResult setBool(Var var, bool value) {
    return svc_config->set_bool(mod_ctx, handle(var), value);
}

ModResult setInt(Var var, int64_t value) {
    return svc_config->set_int(mod_ctx, handle(var), value);
}

ModResult setString(Var var, const std::string& value) {
    return svc_config->set_string(mod_ctx, handle(var), value.c_str());
}

bool hostBool(const char* key, bool fallback) {
    // Each lookup allocates a host slot, so cache the handles.
    static std::unordered_map<std::string, ConfigVarHandle> s_hostHandles;
    if (!SERVICE_HAS(svc_config, ConfigService, find_host_var) ||
        svc_config->find_host_var == nullptr)
    {
        return fallback;
    }
    auto it = s_hostHandles.find(key);
    if (it == s_hostHandles.end()) {
        ConfigVarHandle found = 0;
        if (svc_config->find_host_var(mod_ctx, key, CONFIG_VAR_BOOL, &found) != MOD_OK) {
            found = 0;
        }
        it = s_hostHandles.emplace(key, found).first;
    }
    bool value = fallback;
    if (it->second == 0 || svc_config->get_bool(mod_ctx, it->second, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

double hostFloat(const char* key, double fallback) {
    static std::unordered_map<std::string, ConfigVarHandle> s_hostHandles;
    if (!SERVICE_HAS(svc_config, ConfigService, find_host_var) ||
        svc_config->find_host_var == nullptr)
    {
        return fallback;
    }
    auto it = s_hostHandles.find(key);
    if (it == s_hostHandles.end()) {
        ConfigVarHandle found = 0;
        if (svc_config->find_host_var(mod_ctx, key, CONFIG_VAR_FLOAT, &found) != MOD_OK) {
            found = 0;
        }
        it = s_hostHandles.emplace(key, found).first;
    }
    double value = fallback;
    if (it->second == 0 || svc_config->get_float(mod_ctx, it->second, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

}  // namespace twili::config
