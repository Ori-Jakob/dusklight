#pragma once

#include "enemy/EnemySync.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

class fopAc_ac_c;

// Shared enemy damage: teammates' hits weaken our copy of an enemy; deaths stay with enemy_sync.
namespace twili::enemy_damage {

// ENEMY_DAMAGE carries its own version.
inline constexpr int kVersion = 1;
inline constexpr size_t kMaxHitsPerPacket = 16;

struct Hit {
    enemy_sync::SpawnKey key;
    uint16_t dmg;
    // The sender's applied health percent for this enemy.
    uint16_t pct;
    // The sender's health after the hit in 100% units, for logs.
    int16_t hpAfter;
};

// fopAc_Create returned cPhs_COMPLEATE_e; after enemy_sync::onActorCreated.
void onActorCreated(fopAc_ac_c* actor);
void onActorDeleted(fopAc_ac_c* actor);
// cc_at_check lowered this actor's health.
void onLocalHit(fopAc_ac_c* actor);

// Measures our own damage since the last call and sends it; right after enemy_sync::flush().
void poll();
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Applies pending remote damage.
void tick();
void resetSession();
void shutdown();

struct Stats {
    uint32_t sent = 0;
    uint32_t received = 0;
    // Floored ones included.
    uint32_t applied = 0;
    uint32_t floored = 0;
    // Never matched, or the copy was dying, at its floor or not shared.
    uint32_t dropped = 0;
};
const Stats& stats();

// Autotest.
void noteLocalHitForTest(fopAc_ac_c* actor);
bool sendForTest(fopAc_ac_c* actor, int dmg, int pct);

namespace detail {
bool sendHits(const char* stage, int layer, const Hit* hits, size_t count);
void queueRemote(const Hit& hit);
}  // namespace detail

}  // namespace twili::enemy_damage
