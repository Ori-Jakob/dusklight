#include "fx/RemoteTransformFx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace twili {

TransformFxPlan planTransformFx(const RemoteTransformFx& tf, bool bodyWolf, bool modelSwap) {
    TransformFxPlan plan;
    if (!tf.active()) {
        return plan;
    }
    plan.active = true;

    if (tf.postSwap()) {
        // The new body plays its clip
        if (bodyWolf) {
            plan.slot[0] = TfEmitter::AtowA;
            plan.slot[1] = TfEmitter::AtowB;
        } else {
            plan.slot[1] = TfEmitter::WtoaA;
        }
        plan.anchor = TfAnchor::Frozen;
        plan.bodyTev = true;
        plan.hatScale = !bodyWolf;
        return plan;
    }

    // The old body plays its clip, then only its silhouette shows while the model is swapped.
    if (bodyWolf) {
        plan.slot[0] = TfEmitter::WtoaB;
    } else {
        plan.slot[0] = TfEmitter::AtowA;
        plan.slot[1] = TfEmitter::AtowB;
    }
    if (modelSwap) {
        plan.anchor = TfAnchor::Frozen;
        plan.silhouette = true;
        // daAlink_c::draw
        plan.silhouetteTev = tf.tev > 0 ? 255 : -255;
        plan.silhouetteZ = bodyWolf ? 0.0f : 30.0f;
    } else {
        plan.anchor = TfAnchor::LiveJoint2;
        plan.bodyTev = true;
        plan.hatScale = !bodyWolf;
    }
    return plan;
}

static int32_t quantizeTf(float value, float scale) {
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp<long long>(
        std::llround(static_cast<double>(value) * scale), -2000000000LL, 2000000000LL));
}

void encodeTransformFx(const RemoteTransformFx& tf, int32_t out[6]) {
    std::fill(out, out + 6, 0);
    if (!tf.active()) {
        return;
    }
    out[0] = tf.flags & kTfWireMask;
    out[1] = tf.tev;
    out[2] = quantizeTf(tf.hatScale, kRatioScale);
    for (int i = 0; i < 3; i++) {
        out[3 + i] = quantizeTf(tf.anchor[i], kPosScale);
    }
}

RemoteTransformFx decodeTransformFx(const int32_t in[6], float posX, float posY, float posZ) {
    RemoteTransformFx tf;  // nothing survives an inactive state
    const uint8_t flags = static_cast<uint8_t>(in[0] & kTfWireMask);
    if ((flags & kTfActive) == 0) {
        return tf;
    }
    tf.flags = flags;
    tf.tev = static_cast<int16_t>(std::clamp(in[1], -255, 255));
    tf.hatScale = std::clamp(in[2] / kRatioScale, -2.0f, 2.0f);
    const float anchor[3] = {in[3] / kPosScale, in[4] / kPosScale, in[5] / kPosScale};
    const float dx = anchor[0] - posX;
    const float dy = anchor[1] - posY;
    const float dz = anchor[2] - posZ;
    const float distSq = dx * dx + dy * dy + dz * dz;
    if (std::isfinite(distSq) && distSq <= kTfMaxAnchorDist * kTfMaxAnchorDist) {
        std::copy(anchor, anchor + 3, tf.anchor);
        tf.flags |= kTfAnchorValid;
    }
    return tf;
}

namespace {

bool failTf(std::string& why, const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    why = text;
    return false;
}

struct PlanRow {
    uint8_t flags;
    bool bodyWolf, modelSwap;
    TfEmitter slot0, slot1;
    TfAnchor anchor;
    bool silhouette;
    float silhouetteZ;
    bool bodyTev, hatScale;
};

}  // namespace

bool runTransformFxSelfTest(std::string& why) {
    using E = TfEmitter;
    using A = TfAnchor;
    constexpr uint8_t kA = kTfActive;
    constexpr uint8_t kC = kTfActive | kTfPostSwap;
    // setMetamorphoseEffect and draw() for each stage of the sender's transformation.
    static const PlanRow rows[] = {
        {0, false, false, E::None, E::None, A::None, false, 0.0f, false, false},
        {0, true, true, E::None, E::None, A::None, false, 0.0f, false, false},
        {kTfPostSwap | kTfToWolf, true, false, E::None, E::None, A::None, false, 0.0f, false,
         false},  // not active: nothing
        {kA | kTfToWolf, false, false, E::AtowA, E::AtowB, A::LiveJoint2, false, 0.0f, true, true},
        {kA, true, false, E::WtoaB, E::None, A::LiveJoint2, false, 0.0f, true, false},
        {kA | kTfToWolf, false, true, E::AtowA, E::AtowB, A::Frozen, true, 30.0f, false, false},
        {kA, true, true, E::WtoaB, E::None, A::Frozen, true, 0.0f, false, false},
        {kC | kTfToWolf, true, false, E::AtowA, E::AtowB, A::Frozen, false, 0.0f, true, false},
        {kC, false, false, E::None, E::WtoaA, A::Frozen, false, 0.0f, true, true},
        {kC, false, true, E::None, E::WtoaA, A::Frozen, false, 0.0f, true, true},
        {kC | kTfToWolf, true, true, E::AtowA, E::AtowB, A::Frozen, false, 0.0f, true, false},
    };
    for (size_t i = 0; i < std::size(rows); i++) {
        const PlanRow& r = rows[i];
        RemoteTransformFx tf;
        tf.flags = r.flags;
        tf.tev = -40;
        const TransformFxPlan p = planTransformFx(tf, r.bodyWolf, r.modelSwap);
        if (p.active != ((r.flags & kTfActive) != 0) || p.slot[0] != r.slot0 ||
            p.slot[1] != r.slot1 || p.anchor != r.anchor || p.silhouette != r.silhouette ||
            p.silhouetteZ != r.silhouetteZ || p.bodyTev != r.bodyTev || p.hatScale != r.hatScale)
        {
            return failTf(why, "plan row %zu (flags %u, wolf %d, swap %d): active %d slots %d/%d "
                          "anchor %d silhouette %d z %.0f tev %d hat %d", i, r.flags, r.bodyWolf,
                          r.modelSwap, p.active, static_cast<int>(p.slot[0]),
                          static_cast<int>(p.slot[1]), static_cast<int>(p.anchor), p.silhouette,
                          p.silhouetteZ, p.bodyTev, p.hatScale);
        }
    }

    // The silhouette is white only for a positive value.
    const int16_t tevs[] = {-64, 0, 1, 64};
    const int16_t want[] = {-255, -255, 255, 255};
    for (int i = 0; i < 4; i++) {
        RemoteTransformFx tf;
        tf.flags = kTfActive;
        tf.tev = tevs[i];
        const int16_t got = planTransformFx(tf, false, true).silhouetteTev;
        if (got != want[i]) {
            return failTf(why, "silhouette colour for tev %d: %d (want %d)", tevs[i], got, want[i]);
        }
    }

    RemoteTransformFx in;
    in.flags = kTfActive | kTfPostSwap | kTfToWolf;
    in.tev = -37;
    in.hatScale = 0.625f;
    in.anchor[0] = -1234.5f;
    in.anchor[1] = 310.25f;
    in.anchor[2] = 8000.125f;
    int32_t wire[6];
    encodeTransformFx(in, wire);
    RemoteTransformFx out = decodeTransformFx(wire, -1200.0f, 250.0f, 8050.0f);
    if (out.flags != (in.flags | kTfAnchorValid) || out.tev != in.tev ||
        out.hatScale != in.hatScale || out.anchor[0] != in.anchor[0] ||
        out.anchor[1] != in.anchor[1] || out.anchor[2] != in.anchor[2])
    {
        return failTf(why, "round trip: flags %u tev %d hat %.3f anchor %.2f/%.2f/%.2f", out.flags,
                      out.tev, out.hatScale, out.anchor[0], out.anchor[1], out.anchor[2]);
    }
    // The anchor far from the sender is dropped, the rest kept.
    out = decodeTransformFx(wire, 0.0f, 0.0f, 0.0f);
    if ((out.flags & kTfAnchorValid) != 0 || out.anchor[0] != 0.0f || out.tev != in.tev) {
        return failTf(why, "far anchor kept: flags %u anchor %.1f", out.flags, out.anchor[0]);
    }
    const int32_t inactive[6] = {kTfPostSwap | kTfToWolf, -64, 512, 8, 8, 8};
    out = decodeTransformFx(inactive, 1.0f, 1.0f, 1.0f);
    if (out.flags != 0 || out.tev != 0 || out.hatScale != 1.0f || out.anchor[0] != 0.0f) {
        return failTf(why, "inactive state kept flags %u tev %d hat %.2f", out.flags, out.tev,
                      out.hatScale);
    }
    encodeTransformFx(RemoteTransformFx{}, wire);
    for (int32_t v : wire) {
        if (v != 0) {
            return failTf(why, "inactive state encodes non-zero");
        }
    }
    const int32_t junk[6] = {255, 99999, 99999, 2147483647, 0, 0};
    out = decodeTransformFx(junk, 0.0f, 0.0f, 0.0f);
    if (out.flags != kTfWireMask || out.tev != 255 || out.hatScale != 2.0f) {
        return failTf(why, "junk: flags %u tev %d hat %.2f", out.flags, out.tev, out.hatScale);
    }
    const int32_t junkLow[6] = {kTfActive, -99999, -99999, 0, 0, 0};
    out = decodeTransformFx(junkLow, 0.0f, 0.0f, 0.0f);
    if (out.tev != -255 || out.hatScale != -2.0f || (out.flags & kTfAnchorValid) == 0) {
        return failTf(why, "junk low: tev %d hat %.2f flags %u", out.tev, out.hatScale, out.flags);
    }
    return true;
}

}  // namespace twili

