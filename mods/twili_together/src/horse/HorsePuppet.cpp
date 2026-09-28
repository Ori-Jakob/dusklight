#include "horse/HorsePuppet.hpp"

#include "actors/DummyHorse.hpp"

namespace twili::horse {

void applyPuppetPose(
    uint32_t clientId, const RemoteHorsePose& pose, float frameAlpha, bool snapped) {
    if (daDummyHorse_c* horse = FindDummyHorse(clientId)) {
        horse->applyRemotePose(pose, frameAlpha, snapped);
    }
}

bool riderSeat(uint32_t clientId, const RemoteHorseRider& rider, cXyz& out) {
    daDummyHorse_c* horse = FindDummyHorse(clientId);
    return horse != nullptr && horse->riderWorldPos(rider, out);
}

void applyRider(uint32_t clientId, daAlink_c& rider, const RemoteHorsePose& pose) {
    if (daDummyHorse_c* horse = FindDummyHorse(clientId)) {
        horse->applyRider(rider, pose);
    }
}

uint32_t riddenShadowId(uint32_t clientId) {
    const daDummyHorse_c* horse = FindDummyHorse(clientId);
    return horse != nullptr && horse->isRidden() ? horse->getShadowID() : 0;
}

bool tagInfo(uint32_t clientId, TagInfo& out) {
    const daDummyHorse_c* horse = FindDummyHorse(clientId);
    if (horse == nullptr || !horse->isShown()) {
        return false;
    }
    out.ridden = horse->isRidden();
    out.pos[0] = horse->current.pos.x;
    out.pos[1] = horse->current.pos.y;
    out.pos[2] = horse->current.pos.z;
    out.eye[0] = horse->eyePos.x;
    out.eye[1] = horse->eyePos.y;
    out.eye[2] = horse->eyePos.z;
    return true;
}

}  // namespace twili::horse
