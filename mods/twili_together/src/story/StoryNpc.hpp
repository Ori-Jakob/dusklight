#pragma once

// NPC cutscenes: a teammate joins through its own copy of the NPC ordering the same entry.

#include "core/SpawnKey.hpp"

#include <cstdint>
#include <string>

class daNpcT_c;
class fopAc_ac_c;

namespace twili::story::npc {

// A daNpcT_c subclass that keeps the base evtOrder (every one but npc_hoz).
bool isNpcT(int16_t profile);
// Allowlisted (profile, event): only these are joined.
bool joinable(int16_t profile, const std::string& event);
// The randomizer hooks the NPCs' event choices: equal flags need not mean the same event.
bool joinsOff();
// FNV-64 of the synced event flags and memory switches: what an NPC decides on.
uint64_t storyDigest();

// daNpcT_c::evtOrder pre-hook: remembers which table entry each NPC ordered, and places a join.
void beforeEvtOrder(daNpcT_c* npc);
// The table index `actor` last ordered, -1 if none.
int orderedIndex(const fopAc_ac_c* actor);
// The event name at `index` of the NPC's table, "" if none (index from a same-build teammate).
std::string eventName(const daNpcT_c* npc, int index);

// A live daNpcT_c of that spawn in the stay room, not in an event, or nullptr.
daNpcT_c* findNpc(const SpawnKey& key);
// The next evtOrder of `npc` orders table entry `index`; 0 cancels.
void placeOrder(daNpcT_c* npc, int index);
bool orderPending();

#if TWILI_ENABLE_AUTOTEST
void allowForTest(int16_t profile, const std::string& event);
void perturbDigestForTest(bool on);
#endif

}  // namespace twili::story::npc
