#pragma once

#include <mods/svc/hook.hpp>

#include <cstdint>
#include <string>

// Hooks are installed per group; no replace-hooks, so other mods can share every target.
namespace twili::hooks {

// Observers snapshot in a kObserve pre-hook (before anyone can skip) and decide in the post-hook.
inline constexpr int32_t kObserve = 100;
inline constexpr int32_t kDefault = 0;
inline constexpr int32_t kLate = -100;

enum class Group : uint8_t {
    Core,
    Fx,
    Pvp,
    Enemy,
    EnemyDamage,
    EnemyCount,
    Story,
    Map,
    Autotest,
    Count,
};

// Core failing is fatal; a failed feature group stays inactive and its callbacks check active().
ModResult install(Group group, std::string& error);
bool active(Group group);

template <class Entry>
ModResult addPre(HookPreFn callback, int32_t priority, const char* what, std::string& error) {
    HookOptions options = HOOK_OPTIONS_INIT;
    options.priority = priority;
    const ModResult result = mods::hook::add_pre<Entry>(callback, &options);
    if (result != MOD_OK) {
        error += std::string{what} + " pre-hook failed (" + std::to_string(result) + ")";
    }
    return result;
}

template <class Entry>
ModResult addPost(HookPostFn callback, int32_t priority, const char* what, std::string& error) {
    HookOptions options = HOOK_OPTIONS_INIT;
    options.priority = priority;
    const ModResult result = mods::hook::add_post<Entry>(callback, &options);
    if (result != MOD_OK) {
        error += std::string{what} + " post-hook failed (" + std::to_string(result) + ")";
    }
    return result;
}

// Pushed by an enclosing function's pre-hook, popped by its post-hook, read by callee taps.
enum class ScopeKind : uint8_t {
    None,
    CutReverse,
    JumpLand,
    Hookshot,
    IronBall,
    BombArrow,
    PlantedHit,
    EnemyDamage,
    StagePlaced,
    DmapDraw,
    FmapDraw,
};

struct ScopeEntry {
    ScopeKind kind = ScopeKind::None;
    const void* owner = nullptr;
};

class Scope {
public:
    static void push(ScopeKind kind, const void* owner);
    static void pop(ScopeKind kind, const void* owner);
    static ScopeEntry top();
    // Owner of the innermost entry if it is of `kind`.
    static const void* owner(ScopeKind kind);
    static bool inside(ScopeKind kind) { return owner(kind) != nullptr; }
    static void clear();
};

class ScopeGuard {
public:
    ScopeGuard(ScopeKind kind, const void* owner) : mKind(kind), mOwner(owner) {
        Scope::push(kind, owner);
    }
    ~ScopeGuard() { Scope::pop(mKind, mOwner); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    ScopeKind mKind;
    const void* mOwner;
};

}  // namespace twili::hooks
