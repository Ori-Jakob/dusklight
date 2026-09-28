#pragma once

#include "f_pc/f_pc_base.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

class fopAc_ac_c;
class e_s1_class;

// Enemy-death sync (room setting syncNPCs, "Sync Enemy Deaths"): when a teammate in our stage and
// layer defeats an allowlisted enemy, our copy is removed with a drop-less death puff.
namespace twili::enemy_sync {

// Identifies one enemy on every client: its spawn data, read before the engine frees it.
struct SpawnKey {
    int16_t procName;
    int8_t roomNo;
    uint32_t params;
    uint16_t setId;
    float home[3];
};

struct Kill {
    SpawnKey key;
    // The killer's DISAPPEAR size and type, for our puff.
    uint8_t fxSize = 10;
    uint8_t fxType = 0;
    // The killer set the enemy's dSv_zoneActor_c bit.
    bool zoneActor = false;
};

// ENEMY_DEFEATED carries its own version.
inline constexpr int kVersion = 1;
inline constexpr size_t kMaxKillsPerPacket = 16;

// Hook entry points.
// fopAc_Create returned cPhs_COMPLEATE_e; the append is freed right after.
void onActorCreated(fopAc_ac_c* actor);
void onActorDeleted(fopAc_ac_c* actor);
// The actor spawned its own death puff.
void onDisappear(const fopAc_ac_c* actor, uint8_t size, uint8_t type);
// Game logic asked for the deletion (room unloads never come through here). May be null.
void onDeleteRequest(const fopAc_ac_c* actor);
void onDeleteRequest(fpc_ProcID id);
// A death without a puff (Bokoblin falling into the void, Stalkin).
void markDefeated(const fopAc_ac_c* actor);
// Every Shadow Beast of this one's group was just condemned.
void onShadowBeastGroupDown(const e_s1_class* s1);

// Sends the kills reported since the last call; before the stage tracking of the same update.
void flush();
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Applies pending remote kills.
void tick();
void resetSession();
void shutdown();

struct Stats {
    uint32_t sent = 0;
    // Echoes included.
    uint32_t received = 0;
    uint32_t applied = 0;
    // Never matched or never allowed to apply.
    uint32_t expired = 0;
    // Kills of enemies we had killed or removed already.
    uint32_t echoes = 0;
};
const Stats& stats();

namespace detail {
// False (nothing sent) unless we are where the server routes us and a teammate is there too.
bool sendDefeated(const char* stage, int layer, const Kill* kills, size_t count);
// A validated kill from a teammate in our stage and layer.
void queueRemote(const Kill& kill);
}  // namespace detail

}  // namespace twili::enemy_sync
