#pragma once

#include <array>
#include <cstdint>

// Calls of the hooked targets that run every tick or more, for the autotest's cost check.
namespace twili::hooks::perf {

enum class Target : uint8_t {
    SuspendCheck,
    LinkExecute,
    LinkSfx,
    Management,
    ParticleSet,
    PolyColor,
    HitMark,
    HitItemSe,
    Wpillar,
    FxScope,
    LoadModel,
    BombArrow,
    FastCreate,
    DamageCheck,
    TgHitGObj,
    Count,
};

inline std::array<uint32_t, static_cast<size_t>(Target::Count)> g_calls{};

inline void count(Target target) {
    ++g_calls[static_cast<size_t>(target)];
}

const char* targetName(Target target);

}  // namespace twili::hooks::perf
