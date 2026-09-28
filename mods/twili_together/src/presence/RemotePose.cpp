#include "presence/RemotePose.hpp"

#include "core/Host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace twili {
namespace {

constexpr float kMinDelayTicks = 2.0f;
constexpr float kMaxDelayTicks = 8.0f;
constexpr float kJitterDelayGain = 2.0f;
// One late burst must not dominate the jitter estimate.
constexpr float kMaxJitterSample = 8.0f;
constexpr uint32_t kMaxJitterGapTicks = 30;
constexpr float kErrSmoothing = 0.1f;
constexpr float kRateDeadband = 0.5f;
constexpr float kRateGain = 0.1f;
constexpr float kMinRate = 0.9f;
constexpr float kMaxRate = 3.0f;
constexpr float kReanchorTicks = 45.0f;
// Silence that means a pause when seq barely moved over it.
constexpr int kStallTicks = 8;
constexpr double kMaxExtrapolateTicks = 2.0;
constexpr float kTeleportDistanceSq = kTeleportDistance * kTeleportDistance;

float distSq(const LinkPuppetState& a, const LinkPuppetState& b) {
    const float dx = b.posX - a.posX;
    const float dy = b.posY - a.posY;
    const float dz = b.posZ - a.posZ;
    return dx * dx + dy * dy + dz * dz;
}

// The difference wraps to the shorter way round, so 32000 -> -32000 turns through 0x8000.
int16_t lerpAngle(int16_t a, int16_t b, float t) {
    const int16_t d = static_cast<int16_t>(static_cast<uint16_t>(b) - static_cast<uint16_t>(a));
    return static_cast<int16_t>(a + static_cast<int>(std::lround(d * t)));
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

bool horseContinues(const RemoteHorsePose& a, const RemoteHorsePose& b) {
    if (!a.present() || !b.present() || a.epoch != b.epoch) {
        return false;
    }
    const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
    return dx * dx + dy * dy + dz * dz <= kTeleportDistanceSq;
}

// The horse moves between the samples only while it is the same horse, uninterrupted
void blendHorse(const RemoteHorsePose& a, const RemoteHorsePose& b, float t,
                RemoteHorsePose& out) {
    for (int i = 0; i < 3; i++) {
        out.frameNext[i] = a.frame[i];
    }
    if (!horseContinues(a, b)) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        out.pos[i] = lerp(a.pos[i], b.pos[i], t);
        out.angle[i] = lerpAngle(a.angle[i], b.angle[i], t);
        out.tail[i] = lerpAngle(a.tail[i], b.tail[i], t);
        if (a.anm[i] == b.anm[i]) {
            out.frameNext[i] = b.frame[i];
        }
    }
    for (int i = 0; i < 2; i++) {
        if (a.anm[i] == b.anm[i]) {
            out.ratio[i] = lerp(a.ratio[i], b.ratio[i], t);
        }
    }
    out.neckYaw = lerpAngle(a.neckYaw, b.neckYaw, t);
    out.lean = lerpAngle(a.lean, b.lean, t);
    for (int f = 0; f < 4; f++) {
        for (int j = 0; j < 4; j++) {
            out.foot[f][j] = lerpAngle(a.foot[f][j], b.foot[f][j], t);
        }
    }
    const RemoteHorseRider& ra = a.rider;
    const RemoteHorseRider& rb = b.rider;
    if (ra.active && rb.active && ra.rootMode == rb.rootMode) {
        for (int i = 0; i < 3; i++) {
            out.rider.base[i] = lerp(ra.base[i], rb.base[i], t);
            out.rider.off[i] = lerp(ra.off[i], rb.off[i], t);
        }
    }
}

void blendPose(const LinkPuppetState& a, const LinkPuppetState& b, float t, LinkPuppetState& out) {
    out = a;
    out.posX = lerp(a.posX, b.posX, t);
    out.posY = lerp(a.posY, b.posY, t);
    out.posZ = lerp(a.posZ, b.posZ, t);
    out.angleX = lerpAngle(a.angleX, b.angleX, t);
    out.angleY = lerpAngle(a.angleY, b.angleY, t);
    out.angleZ = lerpAngle(a.angleZ, b.angleZ, t);
    out.shapeAngleX = lerpAngle(a.shapeAngleX, b.shapeAngleX, t);
    out.shapeAngleY = lerpAngle(a.shapeAngleY, b.shapeAngleY, t);
    out.shapeAngleZ = lerpAngle(a.shapeAngleZ, b.shapeAngleZ, t);
    out.bodyAngleX = lerpAngle(a.bodyAngleX, b.bodyAngleX, t);
    out.bodyAngleY = lerpAngle(a.bodyAngleY, b.bodyAngleY, t);
    out.bodyAngleZ = lerpAngle(a.bodyAngleZ, b.bodyAngleZ, t);
    out.bodyTwistY = lerpAngle(a.bodyTwistY, b.bodyTwistY, t);
    out.upperBlendRatio = lerp(a.upperBlendRatio, b.upperBlendRatio, t);
    // A pack that switched animation has no in-between frame
    for (int i = 0; i < 3; i++) {
        out.lowerFramesNext[i] = a.lowerFrames[i];
        out.upperFramesNext[i] = a.upperFrames[i];
        if (a.lowerANMs[i] == b.lowerANMs[i]) {
            out.lowerFramesNext[i] = b.lowerFrames[i];
            out.lowerRatios[i] = lerp(a.lowerRatios[i], b.lowerRatios[i], t);
        }
        if (a.upperANMs[i] == b.upperANMs[i]) {
            out.upperFramesNext[i] = b.upperFrames[i];
            out.upperRatios[i] = lerp(a.upperRatios[i], b.upperRatios[i], t);
        }
    }
    // Midna's layers the same way
    const RemoteMidnaPose& am = a.midna;
    const RemoteMidnaPose& bm = b.midna;
    RemoteMidnaPose& om = out.midna;
    om.bodyFrameNext = am.bodyFrame;
    om.upperFrameNext = am.upperFrame;
    om.faceFrameNext = am.faceFrame;
    if (am.mode != kMidnaNone && am.mode == bm.mode) {
        if (am.bodyBck == bm.bodyBck) om.bodyFrameNext = bm.bodyFrame;
        if (am.upperBck == bm.upperBck) om.upperFrameNext = bm.upperFrame;
        if (am.faceBck == bm.faceBck) om.faceFrameNext = bm.faceFrame;
        om.neckX = lerpAngle(am.neckX, bm.neckX, t);
        om.neckY = lerpAngle(am.neckY, bm.neckY, t);
        om.backboneZ = lerpAngle(am.backboneZ, bm.backboneZ, t);
        om.hairTipY = lerpAngle(am.hairTipY, bm.hairTipY, t);
        om.hairTipZ = lerpAngle(am.hairTipZ, bm.hairTipZ, t);
        if (am.hairAimValid && bm.hairAimValid) {
            om.hairAim = lerpAngle(am.hairAim, bm.hairAim, t);
        }
    }
    // Midna's dome grows 25 units a tick
    if ((a.wolfFx.flags & kWolfFxDome) && (b.wolfFx.flags & kWolfFxDome)) {
        out.wolfFx.domeRadius = lerp(a.wolfFx.domeRadius, b.wolfFx.domeRadius, t);
    }
    // An item moves between the samples only while it is the same object doing the same thing
    for (int i = 0; i < kItemFxSlots; i++) {
        const ItemFxSlot& sa = a.itemFx.slots[i];
        const ItemFxSlot& sb = b.itemFx.slots[i];
        if (!sa.active() || sa.kind != sb.kind || sa.id != sb.id || sa.state != sb.state) {
            continue;
        }
        ItemFxSlot& so = out.itemFx.slots[i];
        for (int k = 0; k < 3; k++) {
            so.pos[k] = lerp(sa.pos[k], sb.pos[k], t);
            so.ang[k] = lerpAngle(sa.ang[k], sb.ang[k], t);
        }
    }
    // The clawshot's tips fly while its mode and the other tip's case stay the same
    const RemoteHookshot& ha = a.itemFx.hk;
    const RemoteHookshot& hb = b.itemFx.hk;
    if (ha.active() && ha.mode == hb.mode && ha.sub == hb.sub) {
        RemoteHookshot& ho = out.itemFx.hk;
        for (int k = 0; k < 3; k++) {
            ho.tip[k] = lerp(ha.tip[k], hb.tip[k], t);
            ho.subTip[k] = lerp(ha.subTip[k], hb.subTip[k], t);
        }
        ho.tipAng[0] = lerpAngle(ha.tipAng[0], hb.tipAng[0], t);
        ho.tipAng[1] = lerpAngle(ha.tipAng[1], hb.tipAng[1], t);
        ho.subAng = lerpAngle(ha.subAng, hb.subAng, t);
        ho.tipFrame = lerp(ha.tipFrame, hb.tipFrame, t);
    }
    const RemoteIronBall& ba = a.itemFx.bc;
    const RemoteIronBall& bb = b.itemFx.bc;
    if (ba.mode != 0 && ba.mode == bb.mode) {
        RemoteIronBall& bo = out.itemFx.bc;
        for (int k = 0; k < 3; k++) {
            bo.ball[k] = lerp(ba.ball[k], bb.ball[k], t);
            bo.ballAng[k] = lerpAngle(ba.ballAng[k], bb.ballAng[k], t);
        }
    }
    blendHorse(a.horse, b.horse, t, out.horse);
    out.frameAlpha = t;
}

}  // namespace

float RemotePoseBuffer::targetDelayTicks() const {
    return std::clamp(kMinDelayTicks + kJitterDelayGain * mJitter, kMinDelayTicks, kMaxDelayTicks);
}

void RemotePoseBuffer::push(const RemotePoseSample& sample, double arrivalSec) {
    if (mCount > 0) {
        if (sample.seq <= at(mCount - 1).seq) {
            clear();  // the sender restarted its tick counter
        } else {
            // RFC 3550 interarrival jitter, in ticks
            const double arrivalTicks = (arrivalSec - mLastArrivalSec) / interp::simPace();
            const uint32_t seqTicks = sample.seq - mLastArrivalSeq;
            if (seqTicks <= kMaxJitterGapTicks && arrivalTicks <= kMaxJitterGapTicks) {
                const double d = arrivalTicks - static_cast<double>(seqTicks);
                mJitter +=
                    ((std::min)(static_cast<float>(std::fabs(d)), kMaxJitterSample) - mJitter) /
                    16.0f;
            }
        }
    }
    mLastArrivalSec = arrivalSec;
    mLastArrivalSeq = sample.seq;
    if (mCount == kCapacity) {
        mHead = (mHead + 1) % kCapacity;
        mCount--;
    }
    mRing[(mHead + mCount) % kCapacity] = sample;
    mCount++;
}

bool RemotePoseBuffer::sampleAt(double renderSeq, LinkPuppetState& out,
                                const RemotePoseSample** shown) const {
    if (mCount == 0) {
        return false;
    }
    size_t ia = 0;
    for (size_t i = mCount; i-- > 0;) {
        if (static_cast<double>(at(i).seq) <= renderSeq) {
            ia = i;
            break;
        }
    }
    const RemotePoseSample& a = at(ia);
    *shown = &a;
    if (ia + 1 < mCount) {
        const RemotePoseSample& b = at(ia + 1);
        // A seq gap means the sender skipped ticks that changed nothing
        const float t = static_cast<float>(
            std::clamp(renderSeq - (static_cast<double>(b.seq) - 1.0), 0.0, 1.0));
        if (a.epoch == b.epoch && distSq(a.state, b.state) <= kTeleportDistanceSq) {
            blendPose(a.state, b.state, t, out);
        } else {
            out = a.state;  // never a pose between the two sides of a teleport
        }
        return true;
    }

    out = a.state;
    const double ahead = (std::min)(renderSeq - static_cast<double>(a.seq), kMaxExtrapolateTicks);
    if (ahead > 0.0 && mCount >= 2) {
        const RemotePoseSample& p = at(mCount - 2);
        if (p.epoch == a.epoch && a.seq - p.seq == 1 &&
            distSq(p.state, a.state) <= kTeleportDistanceSq)
        {
            const float k = static_cast<float>(ahead);
            out.posX += (a.state.posX - p.state.posX) * k;
            out.posY += (a.state.posY - p.state.posY) * k;
            out.posZ += (a.state.posZ - p.state.posZ) * k;
            // The horse carries on with its rider, or the rider would drift off the saddle.
            if (horseContinues(p.state.horse, a.state.horse)) {
                for (int i = 0; i < 3; i++) {
                    out.horse.pos[i] += (a.state.horse.pos[i] - p.state.horse.pos[i]) * k;
                }
            }
        }
    }
    return true;
}

RemotePoseEval RemotePoseBuffer::advance(RemotePlayout& p, LinkPuppetState& out) const {
    RemotePoseEval ev;
    if (mCount == 0) {
        return ev;
    }
    const double oldest = at(0).seq;
    const uint32_t newestSeq = at(mCount - 1).seq;
    const double target = static_cast<double>(newestSeq) - targetDelayTicks();

    bool reanchor = !p.started;
    bool resumed = false;
    if (newestSeq != p.lastNewestSeq) {
        // Nothing arrived for a while and the sender's clock barely moved
        const uint32_t advanced = newestSeq - p.lastNewestSeq;
        resumed = p.started && p.ticksSinceNewData > kStallTicks &&
                  advanced * 2 < static_cast<uint32_t>(p.ticksSinceNewData);
        p.lastNewestSeq = newestSeq;
        p.ticksSinceNewData = 0;
    } else {
        p.ticksSinceNewData++;
    }
    // Against where this tick lands at 1x, so a steady stream plays exactly at the target delay.
    const float err = static_cast<float>(target - (p.renderSeq + 1.0));
    reanchor |= p.started && std::fabs(err) > kReanchorTicks;

    if (reanchor) {
        p.renderSeq = target;
        p.holdSeq = 0.0;
        p.errSmooth = 0.0f;
        p.started = true;
        ev.snapped = true;
    } else if (resumed) {
        p.holdSeq = (std::max)(p.renderSeq, p.holdSeq);
        p.renderSeq = target;
        p.errSmooth = 0.0f;
    } else {
        p.errSmooth += (err - p.errSmooth) * kErrSmoothing;
        float rate = 1.0f;
        if (std::fabs(p.errSmooth) > kRateDeadband) {
            rate = std::clamp(1.0f + kRateGain * p.errSmooth, kMinRate, kMaxRate);
        }
        p.renderSeq += rate;
    }
    p.renderSeq = std::clamp(p.renderSeq, (std::min)(oldest, target),
                             static_cast<double>(newestSeq) + kMaxExtrapolateTicks);

    const RemotePoseSample* shown = nullptr;
    sampleAt((std::max)(p.renderSeq, p.holdSeq), out, &shown);
    const float dx = out.posX - p.lastPos[0];
    const float dy = out.posY - p.lastPos[1];
    const float dz = out.posZ - p.lastPos[2];
    if (p.havePos &&
        (shown->epoch != p.epoch || dx * dx + dy * dy + dz * dz > kTeleportDistanceSq))
    {
        ev.snapped = true;
    }
    p.epoch = shown->epoch;
    p.lastPos[0] = out.posX;
    p.lastPos[1] = out.posY;
    p.lastPos[2] = out.posZ;
    p.havePos = true;
    ev.valid = true;
    return ev;
}

namespace {

bool failCheck(std::string& why, const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    why = text;
    return false;
}

RemotePoseSample poseSample(uint32_t seq, float posX, uint8_t epoch = 0) {
    RemotePoseSample sample;
    sample.seq = seq;
    sample.epoch = epoch;
    sample.state.posX = posX;
    return sample;
}

double tickSec(int tick) {
    return tick * static_cast<double>(interp::simPace());
}

bool checkBunchedArrival(std::string& why) {
    RemotePoseBuffer buf;
    RemotePlayout playout;
    uint32_t seq = 0;
    float lastX = 0.0f;
    for (int tick = 0; tick < 300; tick++) {
        if (tick % 2 == 0) {
            for (int i = 0; i < 2; i++) {
                seq++;
                buf.push(poseSample(seq, seq * 10.0f), tickSec(tick));
            }
        }
        LinkPuppetState out;
        buf.advance(playout, out);
        if (tick >= 150 && std::fabs(out.posX - lastX - 10.0f) > 0.05f) {
            return failCheck(why, "bunched arrival: tick %d moved %.2f instead of 10 "
                             "(delay %.2f)", tick, out.posX - lastX, buf.targetDelayTicks());
        }
        lastX = out.posX;
    }
    return true;
}

bool checkAngleWrap(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    a.state.angleY = a.state.shapeAngleY = 32000;
    b.state.angleY = b.state.shapeAngleY = -32000;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    if (std::abs(static_cast<int>(out.angleY)) <= 32000 ||
        std::abs(static_cast<int>(out.shapeAngleY)) <= 32000)
    {
        return failCheck(why, "angle wrap: 32000 -> -32000 at t=0.5 gave %d/%d, the long way",
                         out.angleY, out.shapeAngleY);
    }
    return true;
}

// A new epoch and a same-epoch jump of 2000 units both appear on their own tick
bool checkTeleports(std::string& why) {
    for (int sameEpoch = 0; sameEpoch < 2; sameEpoch++) {
        RemotePoseBuffer buf;
        RemotePlayout playout;
        int snaps = 0;
        for (int tick = 0; tick < 90; tick++) {
            const uint32_t seq = static_cast<uint32_t>(tick) + 1;
            const bool after = seq > 40;
            const uint8_t epoch = (after && !sameEpoch) ? 1 : 0;
            buf.push(poseSample(seq, (after ? 2000.0f : 0.0f) + seq, epoch), tickSec(tick));
            LinkPuppetState out;
            const RemotePoseEval ev = buf.advance(playout, out);
            if (out.posX > 100.0f && out.posX < 2000.0f) {
                return failCheck(why, "teleport (%s): tick %d shows x=%.1f between the two sides",
                                 sameEpoch ? "distance" : "epoch", tick, out.posX);
            }
            if (tick > 0 && ev.snapped) {
                snaps++;
            }
        }
        if (snaps != 1) {
            return failCheck(why, "teleport (%s): %d snaps reported, expected 1",
                             sameEpoch ? "distance" : "epoch", snaps);
        }
    }
    return true;
}

bool checkGapHold(std::string& why) {
    RemotePoseBuffer buf;
    buf.push(poseSample(10, 0.0f), 0.0);
    buf.push(poseSample(20, 100.0f), tickSec(10));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(15.0, out, &shown);
    const float held = out.posX;
    buf.sampleAt(19.5, out, &shown);
    if (std::fabs(held) > 0.01f || std::fabs(out.posX - 50.0f) > 0.01f) {
        return failCheck(why, "seq gap: x=%.2f at 15 (want 0) and %.2f at 19.5 (want 50)",
                         held, out.posX);
    }
    return true;
}

bool checkExtrapolation(std::string& why) {
    RemotePoseBuffer buf;
    buf.push(poseSample(1, 0.0f), 0.0);
    buf.push(poseSample(2, 10.0f), tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(3.0, out, &shown);
    const float oneTick = out.posX;
    buf.sampleAt(10.0, out, &shown);
    if (std::fabs(oneTick - 20.0f) > 0.01f || std::fabs(out.posX - 30.0f) > 0.01f) {
        return failCheck(why, "underrun: x=%.2f one tick past the newest (want 20), %.2f "
                         "eight ticks past (want 30, capped at two)", oneTick, out.posX);
    }
    return true;
}

// Warms a playout up on a steady stream of `ticks` samples, one per tick.
void warmUp(RemotePoseBuffer& buf, RemotePlayout& playout, uint32_t& seq, int ticks) {
    for (int tick = 0; tick < ticks; tick++) {
        seq++;
        buf.push(poseSample(seq, seq * 10.0f), tickSec(static_cast<int>(seq)));
        LinkPuppetState out;
        buf.advance(playout, out);
    }
}

// A steady stream plays exactly the target delay behind the newest sample, at 1x from the start
bool checkSteadyDelay(std::string& why) {
    RemotePoseBuffer buf;
    RemotePlayout playout;
    float lastX = 0.0f;
    for (uint32_t seq = 1; seq <= 90; seq++) {
        buf.push(poseSample(seq, seq * 10.0f), tickSec(static_cast<int>(seq)));
        LinkPuppetState out;
        buf.advance(playout, out);
        const double lag = seq - playout.renderSeq;
        if (std::fabs(lag - buf.targetDelayTicks()) > 0.05 ||
            (seq > kMinDelayTicks + 1.0f && std::fabs(out.posX - lastX - 10.0f) > 0.05f))
        {
            return failCheck(why, "steady stream: tick %u runs %.2f ticks behind (target %.2f) "
                             "and moved %.2f (want 10)", seq, lag, buf.targetDelayTicks(),
                             out.posX - lastX);
        }
        lastX = out.posX;
    }
    return true;
}

// The sender pauses its game for two seconds while running, then carries on.
bool checkSenderPause(std::string& why) {
    RemotePoseBuffer buf;
    RemotePlayout playout;
    uint32_t seq = 0;
    warmUp(buf, playout, seq, 60);
    LinkPuppetState out;
    for (int tick = 0; tick < 60; tick++) {
        buf.advance(playout, out);
    }
    float lastX = out.posX;
    bool moving = false;
    for (int tick = 0; tick < 30; tick++) {
        seq++;
        buf.push(poseSample(seq, seq * 10.0f), tickSec(static_cast<int>(seq) + 60));
        if (buf.targetDelayTicks() > kMinDelayTicks + 0.05f) {
            return failCheck(why, "sender pause: counted as jitter (delay %.2f ticks)",
                             buf.targetDelayTicks());
        }
        const RemotePoseEval ev = buf.advance(playout, out);
        const float step = out.posX - lastX;
        const bool held = std::fabs(step) <= 0.05f;
        const bool oneTick = std::fabs(step - 10.0f) <= 0.05f;
        if (ev.snapped || !(held || oneTick) || (held && (moving || tick >= kMaxDelayTicks))) {
            return failCheck(why, "sender pause: tick %d after it resumed moved %.2f (want 0 "
                             "while the delay refills, then 10)%s", tick, step,
                             ev.snapped ? " and snapped" : "");
        }
        moving |= oneTick;
        lastX = out.posX;
    }
    return true;
}

// Our own game paused for two seconds while samples kept arriving
bool checkReceiverPause(std::string& why) {
    RemotePoseBuffer buf;
    RemotePlayout playout;
    uint32_t seq = 0;
    warmUp(buf, playout, seq, 60);
    for (int tick = 0; tick < 60; tick++) {
        seq++;
        buf.push(poseSample(seq, seq * 10.0f), tickSec(static_cast<int>(seq)));
    }
    LinkPuppetState out;
    const RemotePoseEval ev = buf.advance(playout, out);
    if (!ev.snapped || out.posX < (seq - kMaxDelayTicks - 1.0f) * 10.0f) {
        return failCheck(why, "receiver pause: snapped=%d x=%.0f, want a jump near x=%.0f",
                         ev.snapped, out.posX, seq * 10.0f);
    }
    return true;
}

bool checkRing(std::string& why) {
    RemotePoseBuffer buf;
    for (uint32_t seq = 1; seq <= 100; seq++) {
        buf.push(poseSample(seq, 0.0f), tickSec(static_cast<int>(seq)));
    }
    if (buf.size() != RemotePoseBuffer::kCapacity) {
        return failCheck(why, "ring: %zu samples after 100 pushes, capacity %zu", buf.size(),
                         RemotePoseBuffer::kCapacity);
    }
    buf.push(poseSample(50, 0.0f), tickSec(101));
    if (buf.size() != 1) {
        return failCheck(why, "ring: seq going backwards left %zu samples, want 1", buf.size());
    }
    return true;
}

bool checkFrameBlend(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    a.state.lowerANMs[0] = b.state.lowerANMs[0] = 5;  // same clip in both samples
    a.state.lowerFrames[0] = 10.0f;
    b.state.lowerFrames[0] = 11.0f;
    a.state.lowerANMs[1] = 6;  // clip switched between the samples
    b.state.lowerANMs[1] = 7;
    a.state.lowerFrames[1] = 3.0f;
    b.state.lowerFrames[1] = 0.0f;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.25, out, &shown);
    if (std::fabs(out.frameAlpha - 0.25f) > 0.001f || out.lowerFrames[0] != 10.0f ||
        out.lowerFramesNext[0] != 11.0f || out.lowerFramesNext[1] != 3.0f)
    {
        return failCheck(why, "frame blend: alpha %.2f, same clip %.1f->%.1f (want 10->11), "
                         "switched clip next %.1f (want 3)", out.frameAlpha, out.lowerFrames[0],
                         out.lowerFramesNext[0], out.lowerFramesNext[1]);
    }
    return true;
}

// Midna's layers blend like the body packs, and her angles take the short way round.
bool checkMidnaBlend(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    a.state.midna.mode = b.state.midna.mode = kMidnaDrawn;
    a.state.midna.bodyBck = b.state.midna.bodyBck = 0x1DC;  // same body clip
    a.state.midna.bodyFrame = 20.0f;
    b.state.midna.bodyFrame = 21.0f;
    a.state.midna.faceBck = 0x1BF;  // face clip switched between the samples
    b.state.midna.faceBck = 0x1C0;
    a.state.midna.faceFrame = 7.0f;
    b.state.midna.faceFrame = 0.0f;
    a.state.midna.neckY = 32000;
    b.state.midna.neckY = -32000;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    const RemoteMidnaPose& m = out.midna;
    if (m.bodyFrame != 20.0f || m.bodyFrameNext != 21.0f || m.faceFrameNext != 7.0f ||
        std::abs(static_cast<int>(m.neckY)) <= 32000)
    {
        return failCheck(why, "midna blend: body %.1f->%.1f (want 20->21), switched face next "
                         "%.1f (want 7), neck 32000 -> -32000 gave %d (want the short way)",
                         m.bodyFrame, m.bodyFrameNext, m.faceFrameNext, m.neckY);
    }
    return true;
}

// The dome's radius blends while both samples have it
bool checkWolfFxBlend(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    a.state.wolfFx.flags = kWolfFxDome;
    b.state.wolfFx.flags = kWolfFxDome | kWolfFxLockBlur;
    a.state.wolfFx.domeRadius = 100.0f;
    b.state.wolfFx.domeRadius = 125.0f;
    a.state.wolfFx.lockDashSeq = 4;
    b.state.wolfFx.lockDashSeq = 5;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    const RemoteWolfFx& f = out.wolfFx;
    if (std::fabs(f.domeRadius - 112.5f) > 0.01f || f.flags != kWolfFxDome || f.lockDashSeq != 4) {
        return failCheck(why, "wolf fx blend: radius 100->125 gave %.2f (want 112.5), flags 0x%X "
                         "(want 0x%X), dash seq %d (want 4)", f.domeRadius, f.flags, kWolfFxDome,
                         f.lockDashSeq);
    }
    return true;
}

// An item slot blends while it holds the same object in the same state
bool checkItemFxBlend(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    for (RemotePoseSample* s : {&a, &b}) {
        for (int i = 0; i < 3; i++) {
            s->state.itemFx.slots[i].kind = kItemFxArrow;
            s->state.itemFx.slots[i].state = kItemFxArrowFly;
            s->state.itemFx.slots[i].id = 7;
        }
    }
    a.state.itemFx.slots[0].pos[0] = 100.0f;  // same arrow in flight
    b.state.itemFx.slots[0].pos[0] = 260.0f;
    a.state.itemFx.slots[0].ang[1] = 32000;
    b.state.itemFx.slots[0].ang[1] = -32000;
    b.state.itemFx.slots[1].id = 8;  // a new arrow in the slot
    a.state.itemFx.slots[1].pos[0] = 100.0f;
    b.state.itemFx.slots[1].pos[0] = 900.0f;
    b.state.itemFx.slots[2].state = kItemFxArrowStuckBg;  // stuck in between
    a.state.itemFx.slots[2].pos[0] = 100.0f;
    b.state.itemFx.slots[2].pos[0] = 150.0f;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    const ItemFxSlot* s = out.itemFx.slots;
    if (std::fabs(s[0].pos[0] - 180.0f) > 0.01f || std::abs(static_cast<int>(s[0].ang[1])) <= 32000 ||
        s[1].pos[0] != 100.0f || s[1].id != 7 || s[2].pos[0] != 100.0f ||
        s[2].state != kItemFxArrowFly)
    {
        return failCheck(why, "item fx blend: flying %.1f (want 180) angle %d (want the short "
                         "way), new object %.1f id %d (want 100, 7), state change %.1f state %d "
                         "(want 100, fly)", s[0].pos[0], s[0].ang[1], s[1].pos[0], s[1].id,
                         s[2].pos[0], s[2].state);
    }
    return true;
}

bool checkHookBallBlend(std::string& why) {
    RemotePoseBuffer buf;
    RemotePoseSample a = poseSample(1, 0.0f);
    RemotePoseSample b = poseSample(2, 0.0f);
    RemotePoseSample c = poseSample(3, 0.0f);
    a.state.itemFx.hk.mode = b.state.itemFx.hk.mode = kItemFxHookShoot;
    c.state.itemFx.hk.mode = kItemFxHookReturn;
    a.state.itemFx.hk.tip[2] = 100.0f;
    b.state.itemFx.hk.tip[2] = 200.0f;
    c.state.itemFx.hk.tip[2] = 900.0f;
    a.state.itemFx.bc.mode = b.state.itemFx.bc.mode = 5;
    a.state.itemFx.bc.ball[1] = 40.0f;
    b.state.itemFx.bc.ball[1] = 20.0f;
    buf.push(a, 0.0);
    buf.push(b, tickSec(1));
    buf.push(c, tickSec(2));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    const float tip = out.itemFx.hk.tip[2];
    const float ball = out.itemFx.bc.ball[1];
    buf.sampleAt(2.5, out, &shown);
    if (std::fabs(tip - 150.0f) > 0.01f || std::fabs(ball - 30.0f) > 0.01f ||
        out.itemFx.hk.tip[2] != 200.0f || out.itemFx.hk.mode != kItemFxHookShoot)
    {
        return failCheck(why, "hook/ball blend: tip %.1f (want 150), ball %.1f (want 30), tip "
                         "across a mode change %.1f mode %d (want 200, shoot)", tip, ball,
                         out.itemFx.hk.tip[2], out.itemFx.hk.mode);
    }
    return true;
}

bool checkHorseBlend(std::string& why) {
    auto horseAt = [](uint32_t seq, uint8_t epoch, float x, int16_t yaw, uint16_t anm0,
                      float frame0) {
        RemotePoseSample s;
        s.seq = seq;
        s.state.posX = static_cast<float>(seq);
        RemoteHorsePose& h = s.state.horse;
        h.flags = kHorsePresent;
        h.epoch = epoch;
        h.pos[0] = x;
        h.angle[1] = yaw;
        h.anm[0] = anm0;
        h.frame[0] = frame0;
        h.anm[2] = 15;
        h.frame[2] = seq * 2.0f;
        return s;
    };
    RemotePoseBuffer buf;
    buf.push(horseAt(1, 3, 0.0f, 32000, 27, 10.0f), tickSec(1));
    buf.push(horseAt(2, 3, 20.0f, -32000, 27, 11.0f), tickSec(2));
    buf.push(horseAt(3, 4, 300.0f, -32000, 19, 0.0f), tickSec(3));
    LinkPuppetState out;
    const RemotePoseSample* shown = nullptr;
    buf.sampleAt(1.5, out, &shown);
    const RemoteHorsePose h = out.horse;
    const int16_t wantYaw = lerpAngle(32000, -32000, 0.5f);
    if (std::fabs(h.pos[0] - 10.0f) > 0.01f || h.angle[1] != wantYaw ||
        std::fabs(h.frameNext[0] - 11.0f) > 0.01f || std::fabs(h.frame[0] - 10.0f) > 0.01f)
    {
        return failCheck(why, "horse blend: x %.2f (want 10), yaw %d (want %d, the short way), "
                         "frames %.1f->%.1f (want 10->11)", h.pos[0], h.angle[1], wantYaw,
                         h.frame[0], h.frameNext[0]);
    }
    buf.sampleAt(2.5, out, &shown);
    if (out.horse.pos[0] != 20.0f || out.horse.frameNext[0] != 11.0f || out.horse.epoch != 3) {
        return failCheck(why, "horse blend: across a horse epoch change x %.2f (want 20), next "
                         "frame %.1f (want 11), epoch %d (want 3)", out.horse.pos[0],
                         out.horse.frameNext[0], out.horse.epoch);
    }
    // An absent horse in the later sample
    RemotePoseBuffer gone;
    gone.push(horseAt(1, 3, 0.0f, 0, 27, 10.0f), tickSec(1));
    RemotePoseSample none = horseAt(2, 3, 50.0f, 0, 27, 12.0f);
    none.state.horse = RemoteHorsePose{};
    gone.push(none, tickSec(2));
    gone.sampleAt(1.5, out, &shown);
    if (!out.horse.present() || out.horse.pos[0] != 0.0f) {
        return failCheck(why, "horse blend: toward an absent horse present %d x %.2f (want 1, 0)",
                         out.horse.present() ? 1 : 0, out.horse.pos[0]);
    }
    return true;
}

}  // namespace

bool runRemotePoseSelfTest(std::string& why) {
    return checkBunchedArrival(why) && checkAngleWrap(why) && checkTeleports(why) &&
           checkGapHold(why) && checkExtrapolation(why) && checkSteadyDelay(why) &&
           checkSenderPause(why) && checkReceiverPause(why) && checkRing(why) &&
           checkFrameBlend(why) && checkMidnaBlend(why) && checkWolfFxBlend(why) &&
           checkItemFxBlend(why) && checkHookBallBlend(why) && checkHorseBlend(why);
}

}  // namespace twili

