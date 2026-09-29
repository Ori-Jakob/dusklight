#pragma once

#include "enemy/EnemySync.hpp"

#include "f_pc/f_pc_base.h"

#include <cstdint>

class fopAc_ac_c;

// Enemy count multiplier: each client makes the same extra copies of stage-placed regular enemies.
namespace twili::enemy_count {

// fpcSCtRq_Request returned for an actor dStage_actorCreate placed.
void onPlacedRequested(fpc_ProcID id);
// fopAc_Create returned cPhs_COMPLEATE_e; before enemy_sync::onActorCreated.
void onActorCreated(fopAc_ac_c* actor);
void onActorDeleted(fopAc_ac_c* actor);

// An extra's key: its original's key with the extra's index as dup.
bool extraKey(fpc_ProcID id, enemy_sync::SpawnKey& out);
bool isExtra(fpc_ProcID id);
// The live extra `dup` of an original, or fpcM_ERROR_PROCESS_ID_e.
fpc_ProcID extraOf(fpc_ProcID originalId, uint8_t dup);
// The spawn data an extra was made from (autotest checks).
struct ExtraSpawn {
    fpc_ProcID originalId;
    uint32_t params;
    uint16_t setId;
    int16_t angleZ;
    uint32_t switchMask;
    bool switchInAngleZ;
};
bool extraSpawn(fpc_ProcID id, ExtraSpawn& out);

// Spawns decided extras and removes stuck ones; every update, connected or not.
void tick();
// The room's percent while joined, else 100.
int countPercent();
void shutdown();

}  // namespace twili::enemy_count
