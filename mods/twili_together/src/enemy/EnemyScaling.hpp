#pragma once

#include "f_pc/f_pc_base.h"

#include <cstdint>

class fopAc_ac_c;

// Enemy health multiplier: each client applies the room's percent to its own enemies.
namespace twili::enemy_scaling {

// fopAc_Create returned cPhs_COMPLEATE_e.
void onActorCreated(fopAc_ac_c* actor);
void onActorDeleted(fopAc_ac_c* actor);
// Game code set health back to a fixed full value, maybe below the scaled health.
void onHealthReset(fopAc_ac_c* actor);
// Every update, connected or not: a disconnect rescales enemies back to 100%.
void tick();
// The room's percent while joined, else 100.
int healthPercent();
void shutdown();

struct TrackedHealth {
    int16_t baseMax;
    int appliedPct;
};
bool trackedHealth(fpc_ProcID id, TrackedHealth& out);

}  // namespace twili::enemy_scaling
