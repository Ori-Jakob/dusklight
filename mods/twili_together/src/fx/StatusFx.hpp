#pragma once

// Status effects of a player as its daAlink_c shows them

#include "presence/RemotePose.hpp"

#include <cstdint>

namespace twili::statusfx {

// The local player's status, read right after its execute().
void captureLocal(RemoteStatusFx& out);
// What captureLocal returned last (autotest).
const RemoteStatusFx& lastCaptured();
// A new session
void resetSender();

void encode(const RemoteStatusFx& s, int32_t sx[5], int32_t sf[8]);
// Flags masked to the wire bits, the shield burn clamped to 0..120, the sink to +-256 units
RemoteStatusFx decode(const int32_t sx[5], const int32_t sf[8]);

}  // namespace twili::statusfx
