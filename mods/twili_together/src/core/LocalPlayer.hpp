#pragma once

class daAlink_c;

// Checks on the local player shared by toasts, teleport and story follow.
namespace twili::local {

// The local Link once its create() finished.
daAlink_c* liveLink();

// Standing or walking: nothing in the current proc is tied to a ledge, ladder or water surface.
bool isSettledOnGround(daAlink_c* link);

// Settled on a floor that starts no restart (pit, fog, quicksand, lava).
bool onSafeFloor(daAlink_c* link);

// Why the local player cannot be moved now, or nullptr: "not-in-game", "loading", "menu",
// "cutscene", "down", "riding", "busy", "carrying".
const char* localTeleportBlocker();

}  // namespace twili::local
