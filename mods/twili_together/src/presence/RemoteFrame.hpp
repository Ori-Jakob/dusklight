#pragma once

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"

#include <cmath>

namespace twili {

// `frame` inside [0, frame max] of `bck`
inline float normalizeFrame(J3DAnmTransform* bck, float frame) {
    if (!bck || frame <= 0.0f) {
        return 0.0f;
    }

    const s16 maxFrame = bck->getFrameMax();
    if (maxFrame <= 0) {
        return 0.0f;
    }

    const float max = static_cast<float>(maxFrame);
    if (frame > max) {
        frame = std::fmod(frame, max);
    }
    return frame;
}

// The frame between two received samples of the same clip (LinkPuppetState::frameAlpha).
inline float blendRemoteFrame(J3DAnmTransform* bck, float from, float to, float alpha) {
    if (alpha <= 0.0f || !bck) {
        return from;
    }
    float delta = to - from;
    const float max = static_cast<float>(bck->getFrameMax());
    const u8 attr = bck->getAttribute();
    if (max > 0.0f &&
        (attr == J3DFrameCtrl::EMode_LOOP || attr == J3DFrameCtrl::EMode_LOOP_REVERSE))
    {
        if (delta < -max * 0.5f) {
            delta += max;
        } else if (delta > max * 0.5f) {
            delta -= max;
        }
    }
    // More than a few frames in one tick is a restart or a seek
    static constexpr float kMaxBlendFrames = 4.0f;
    if (std::fabs(delta) > kMaxBlendFrames) {
        return alpha < 0.5f ? from : to;
    }
    return from + delta * alpha;  // normalizeFrame wraps a result past the end of a loop
}

}  // namespace twili

