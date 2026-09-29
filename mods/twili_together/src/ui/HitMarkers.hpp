#pragma once

#include <cstdint>
#include <string>

// PvP: a short rising number where our hit landed on another player, from its DAMAGE_RESULT.
namespace twili::ui::hit_markers {

bool install();
void uninstall();

// Near `clientId`'s dummy, `offset` from its feet.
void spawn(uint32_t clientId, const float offset[3], bool blocked, int damage);
// "-1/2" for half a heart, "Hit" for none, "Blocked".
std::string text(bool blocked, int damage);
// Frames that drew at least one marker (autotest).
uint32_t drawnFrames();

}  // namespace twili::ui::hit_markers
