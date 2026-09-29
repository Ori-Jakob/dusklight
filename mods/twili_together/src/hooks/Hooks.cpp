#include "hooks/Hooks.hpp"

#include "core/Log.hpp"

#include <array>

namespace twili::hooks {

// Defined in the hooks_*.cpp files.
ModResult installSave(std::string& error);
ModResult installPlayer(std::string& error);
ModResult installActor(std::string& error);
ModResult installSync(std::string& error);
ModResult installClothes(std::string& error);
ModResult installFx(std::string& error);
ModResult installMap(std::string& error);
ModResult installEnemy(std::string& error);
ModResult installEnemyDamage(std::string& error);
ModResult installStory(std::string& error);
ModResult installPvp(std::string& error);
#if TWILI_ENABLE_AUTOTEST
ModResult installAutotest(std::string& error);
#endif

namespace {

using Installer = ModResult (*)(std::string& error);

ModResult installCore(std::string& error) {
    ModResult result = installSave(error);
    if (result == MOD_OK) {
        result = installPlayer(error);
    }
    if (result == MOD_OK) {
        result = installActor(error);
    }
    if (result == MOD_OK) {
        result = installSync(error);
    }
    if (result == MOD_OK) {
        result = installClothes(error);
    }
    return result;
}

struct GroupInfo {
    const char* name;
    Installer installer;
};

constexpr std::array<GroupInfo, static_cast<size_t>(Group::Count)> kGroups{{
    {"core", installCore},
    {"fx", installFx},
    {"pvp", installPvp},
    {"enemy", installEnemy},
    {"enemy damage", installEnemyDamage},
    {"story", installStory},
    {"map", installMap},
#if TWILI_ENABLE_AUTOTEST
    {"autotest", installAutotest},
#else
    {"autotest", nullptr},
#endif
}};

std::array<bool, static_cast<size_t>(Group::Count)> s_active{};

constexpr size_t kMaxScopeDepth = 16;
std::array<ScopeEntry, kMaxScopeDepth> s_scopes{};
// Also counts pushes past the array so pops stay balanced.
size_t s_scopeDepth = 0;

}  // namespace

ModResult install(Group group, std::string& error) {
    const auto& info = kGroups[static_cast<size_t>(group)];
    auto& active = s_active[static_cast<size_t>(group)];
    if (active) {
        return MOD_OK;
    }
    const ModResult result = info.installer != nullptr ? info.installer(error) : MOD_OK;
    active = result == MOD_OK;
    if (active) {
        TwiliLog.debug("[hooks] group '{}' installed", info.name);
    } else {
        TwiliLog.error("[hooks] group '{}' failed: {}", info.name, error);
    }
    return result;
}

bool active(Group group) {
    return s_active[static_cast<size_t>(group)];
}

void Scope::push(ScopeKind kind, const void* owner) {
    if (s_scopeDepth < kMaxScopeDepth) {
        s_scopes[s_scopeDepth] = {kind, owner};
    } else if (s_scopeDepth == kMaxScopeDepth) {
        TwiliLog.warn("[hooks] scope stack overflow (kind {})", static_cast<int>(kind));
    }
    s_scopeDepth++;
}

void Scope::pop(ScopeKind kind, const void* owner) {
    if (s_scopeDepth == 0) {
        TwiliLog.warn("[hooks] scope pop without push (kind {})", static_cast<int>(kind));
        return;
    }
    s_scopeDepth--;
    if (s_scopeDepth < kMaxScopeDepth) {
        const ScopeEntry entry = s_scopes[s_scopeDepth];
        if (entry.kind != kind || entry.owner != owner) {
            TwiliLog.warn("[hooks] unbalanced scope pop (kind {}, expected {})",
                static_cast<int>(kind), static_cast<int>(entry.kind));
        }
        s_scopes[s_scopeDepth] = {};
    }
}

ScopeEntry Scope::top() {
    if (s_scopeDepth == 0 || s_scopeDepth > kMaxScopeDepth) {
        return {};
    }
    return s_scopes[s_scopeDepth - 1];
}

const void* Scope::owner(ScopeKind kind) {
    const ScopeEntry entry = top();
    return entry.kind == kind ? entry.owner : nullptr;
}

void Scope::clear() {
    s_scopes = {};
    s_scopeDepth = 0;
}

}  // namespace twili::hooks
