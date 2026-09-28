#pragma once

#include "presence/RemotePose.hpp"

#include <cstdint>

struct cXyz;
class daAlink_c;

// The remote horse puppet as the dummy player sees it; P5 adds the puppet itself.
namespace twili::horse {

// Poses `clientId`'s puppet from the sample the dummy shows (the horse moves first).
void applyPuppetPose(uint32_t clientId, const RemoteHorsePose& pose, float frameAlpha,
    bool snapped);
// Where the rider sits on `clientId`'s puppet; false without one.
bool riderSeat(uint32_t clientId, const RemoteHorseRider& rider, cXyz& out);
// Puts the rider's feet in the stirrups (after the rider's calc).
void applyRider(uint32_t clientId, daAlink_c& rider, const RemoteHorsePose& pose);
// The puppet's real shadow while it is ridden, 0 otherwise.
uint32_t riddenShadowId(uint32_t clientId);

}  // namespace twili::horse
