#include "horse/HorsePuppet.hpp"

// No horse puppets until P5 replaces this file.
namespace twili::horse {

void applyPuppetPose(uint32_t, const RemoteHorsePose&, float, bool) {}

bool riderSeat(uint32_t, const RemoteHorseRider&, cXyz&) {
    return false;
}

void applyRider(uint32_t, daAlink_c&, const RemoteHorsePose&) {}

uint32_t riddenShadowId(uint32_t) {
    return 0;
}

}  // namespace twili::horse
