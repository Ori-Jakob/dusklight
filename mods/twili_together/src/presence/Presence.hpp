#pragma once

class daAlink_c;

namespace twili::presence {

// After the local player's execute: gathers its pose and sends PLAYER_UPDATE.
void captureAndSend(daAlink_c* link);

}  // namespace twili::presence
