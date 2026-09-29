#pragma once

class daAlink_c;

namespace twili::presence {

// After the local player's execute: gathers its pose and sends PLAYER_UPDATE.
void captureAndSend(daAlink_c* link);

// Each session tick: while the player's execute is not running (game paused), a peer who arrives
// gets our last pose as a keyframe instead of nothing.
void resendIdlePose();

}  // namespace twili::presence
