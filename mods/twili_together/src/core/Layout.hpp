#pragma once

// d_save.h is not self-contained.
#include "d/d_com_inf_game.h"

#include <cstdint>
#include <string>

// Hash of the save layouts the raw world-state blobs depend on; peers must match.
namespace twili::layout {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

constexpr uint64_t fnv1a(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xffu;
        hash *= kFnvPrime;
    }
    return hash;
}

constexpr uint64_t saveLayoutHash() {
    uint64_t hash = kFnvOffset;
    for (const uint64_t value : {
             static_cast<uint64_t>(sizeof(dSv_save_c)),
             static_cast<uint64_t>(sizeof(dSv_player_c)),
             static_cast<uint64_t>(sizeof(dSv_memory_c)),
             static_cast<uint64_t>(sizeof(dSv_memory2_c)),
             static_cast<uint64_t>(sizeof(dSv_event_c)),
             static_cast<uint64_t>(sizeof(dSv_danBit_c)),
             static_cast<uint64_t>(sizeof(dSv_zone_c)),
             static_cast<uint64_t>(dSv_save_c::STAGE_MAX),
             static_cast<uint64_t>(dSv_save_c::STAGE2_MAX),
             static_cast<uint64_t>(MAX_EVENTS),
         })
    {
        hash = fnv1a(hash, value);
    }
    return hash;
}

inline constexpr uint64_t kSaveLayoutHash = saveLayoutHash();

inline std::string saveLayoutHex() {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i) {
        out[15 - i] = kDigits[(kSaveLayoutHash >> (i * 4)) & 0xfu];
    }
    return out;
}

}  // namespace twili::layout
