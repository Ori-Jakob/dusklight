#include "fx/PlayerRecolor.hpp"

// No-op until P4 recolours through TextureService: every set stays as on disc.
namespace twili {

uint32_t recolorKey(uint8_t r, uint8_t g, uint8_t b) {
    if (r == 255 && g == 255 && b == 255) {
        return RecolorSet::kPristine;
    }
    return static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
}

void RecolorSet::reset() {
    *this = RecolorSet{};
}

int RecolorSet::bind(RecolorSetId id, J3DModelData* const[2], JKRHeap*, uint32_t clientId) {
    reset();
    mId = id;
    mClientId = clientId;
    return 0;
}

void RecolorSet::apply(uint32_t, int32_t tick) {
    mLastApplyTick = tick;
}

void RecolorSet::probe(RecolorProbe& out) const {
    out.bound = bound();
    out.appliedKey = mAppliedKey;
    out.applyCount = mApplyCount;
    out.lastApplyMs = mLastApplyMs;
    out.texCount = 0;
}

}  // namespace twili
