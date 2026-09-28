#pragma once

#include <cstdint>

class daAlink_c;
class fopAc_ac_c;

namespace twili {

// Runtime proc names of our actor profiles (ActorService); -1 until registered.
extern int16_t g_procDummyPlayer;
extern int16_t g_procDummyHorse;

bool isDummyPlayer(const fopAc_ac_c* actor);
// The local player's daAlink_c, or null.
daAlink_c* localLink();

}  // namespace twili
