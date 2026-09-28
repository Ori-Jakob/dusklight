#pragma once

// The wolf's attack effects as its daAlink_c shows them

#include "presence/RemotePose.hpp"

#include <cstdint>

namespace twili::wolffx {

// mEquipItem while Midna's dome is up (setWolfLockDomeModel, d_a_alink_wolf.inc).
inline constexpr uint16_t kLockDomeItem = 0x109;

// Counts the local wolf's lock-on jumps.
void track();
// The local wolf's attack effects, read right after its execute().
bool captureLocal(const RemoteMidnaPose& midna, RemoteWolfFx& out);
// What captureLocal returned last (autotest).
const RemoteWolfFx& lastCaptured();

void encode(const RemoteWolfFx& f, int32_t wx[4]);
// Flags masked to the wire bits, the radius clamped to kMaxWolfDomeRadius
RemoteWolfFx decode(const int32_t wx[4]);

}  // namespace twili::wolffx
