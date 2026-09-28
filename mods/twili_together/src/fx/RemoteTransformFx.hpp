#pragma once

// A remote player's transformation replayed on its dummy

#include "presence/RemotePose.hpp"

#include <cstdint>
#include <string>

namespace twili {

// The emitters setMetamorphoseEffect sets.
enum class TfEmitter : uint8_t { None, AtowA, AtowB, WtoaA, WtoaB };

// Bit of `e` in the autotest's emitter masks.
constexpr uint8_t tfEmitterBit(TfEmitter e) {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(e));
}

// Where the emitters sit
enum class TfAnchor : uint8_t { None, LiveJoint2, Frozen };

struct TransformFxPlan {
    bool active = false;
    // The sender's emitter slots field_0x31f8 and field_0x31fc
    TfEmitter slot[2] = {TfEmitter::None, TfEmitter::None};
    TfAnchor anchor = TfAnchor::None;
    bool silhouette = false;     // only wl_change.bmd is drawn (the model swap)
    int16_t silhouetteTev = 0;   // its colour, +-255 by the sign of the TEV value
    float silhouetteZ = 0.0f;    // pushed forward while the old body is human
    bool bodyTev = false;        // the body is drawn in `tev`
    bool hatScale = false;       // the hat is calced with `hatScale`
};

// `bodyWolf` is the body the dummy has bound this tick
TransformFxPlan planTransformFx(const RemoteTransformFx& tf, bool bodyWolf, bool modelSwap);

// All zero while no transformation runs.
void encodeTransformFx(const RemoteTransformFx& tf, int32_t out[6]);

// Farther than this from the sender's position, a received anchor is not used.
inline constexpr float kTfMaxAnchorDist = 1000.0f;

RemoteTransformFx decodeTransformFx(const int32_t in[6], float posX, float posY, float posZ);

// Checks the planner and the codec (autotest op transformFxSelfTest).
bool runTransformFxSelfTest(std::string& why);

}  // namespace twili
