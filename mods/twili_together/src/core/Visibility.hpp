#pragma once

#include <cstdint>

namespace twili {

struct Client;

// Demos count; talk, compulsory events and doors do not.
bool isCutsceneEvent(bool eventRunning, uint8_t eventMode, uint16_t eventFlags);

// Also true for a moment after one ends.
bool localCutsceneRunning();

bool hideRemotePlayersForCutscene();

bool hideRemoteClientForCutscene(const Client& client);

// -1 follows the real events, 0 or 1 forces the state.
void setCutsceneOverrideForTest(int value);

}  // namespace twili
