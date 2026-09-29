#pragma once

// The local player's bobber rod as peers are to show it

#include "presence/RemotePose.hpp"

#include <cstdint>

class daAlink_c;

namespace twili::fishing {

inline constexpr int kWireSize = 27 + 3 * kFishingLinePoints;

// Our dmg_rod_class this tick (inactive without one).
void captureLocal(daAlink_c* link, RemoteFishing& out);
void encode(const RemoteFishing& f, int32_t out[kWireSize]);
RemoteFishing decode(const int32_t in[kWireSize]);

}  // namespace twili::fishing
