#pragma once

#include <string>

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

// Rooms the warp tool can load; start point -1 needs the room's PLYR chunk.
bool isTeleportableRoom(const char* stage, int roomNo);

// The warp tool knows PLYR point `point` of the room (an unknown one is fatal on load).
bool isKnownEntrance(const char* stage, int roomNo, int point);

// The warp tool's name for the stage and room, else the stage name.
std::string mapName(const char* stage, int roomNo);

}  // namespace twili::local
