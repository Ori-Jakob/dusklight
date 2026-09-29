#include "fx/Fishing.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_mg_rod.h"
#include "f_op/f_op_actor_mng.h"
#include "SSystem/SComponent/c_math.h"

#include <algorithm>
#include <cmath>

namespace twili::fishing {
namespace {

// Where the line samples come from, tip (0) to hook (99); 68 is the bobber
constexpr int kLineIndex[kFishingLinePoints] = {0, 14, 28, 42, 56, 68, 84, 99};
constexpr float kMaxSpan = 3000.0f;

int32_t wirePos(float v) {
    return std::isfinite(v) ? static_cast<int32_t>(std::lround(v * kPosScale)) : 0;
}

int32_t wireRatio(float v) {
    return std::isfinite(v) ? static_cast<int32_t>(std::lround(std::clamp(v, -4.0f, 4.0f) *
                                                               kRatioScale))
                            : 0;
}

void putPos(int32_t* out, const cXyz& p) {
    out[0] = wirePos(p.x);
    out[1] = wirePos(p.y);
    out[2] = wirePos(p.z);
}

void takePos(const int32_t* in, float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = in[i] / kPosScale;
    }
}

}  // namespace

void captureLocal(daAlink_c* link, RemoteFishing& out) {
    out = RemoteFishing{};
    if (link == nullptr || !daPy_py_c::checkFishingRodItem(link->mEquipItem)) {
        return;
    }
    fopAc_ac_c* actor = link->mItemAcKeep.getActor();
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_MG_ROD_e) {
        return;
    }
    const auto* rod = reinterpret_cast<const dmg_rod_class*>(actor);
    if (rod->kind != MG_ROD_KIND_UKI || rod->uki_model == nullptr) {
        return;
    }
    out.active = true;
    out.action = static_cast<uint8_t>(std::clamp<int>(rod->action, 0, 7));
    out.hookKind = rod->hook_kind == 1 ? 1 : 0;
    out.esaKind = static_cast<uint8_t>(std::clamp<int>(rod->esa_kind, 0, 2));
    out.aimDown = rod->action == ACTION_UKI_STANDBY && rod->field_0x1508 < 0.1f;
    out.bobber[0] = rod->hook_pos.x;
    out.bobber[1] = rod->hook_pos.y;
    out.bobber[2] = rod->hook_pos.z;
    const int16_t bobber[6] = {rod->field_0x10a0, rod->field_0x10a2, rod->field_0x1084.y,
                               rod->field_0x1084.x, rod->field_0x108e, rod->field_0x108a};
    std::copy(bobber, bobber + 6, out.bobberAng);
    // uki_main's hook matrix
    const cXyz& hook = actor->current.pos;
    const cXyz toLine = rod->mg_line.pos[95] - hook;
    out.hook[0] = hook.x;
    out.hook[1] = hook.y;
    out.hook[2] = hook.z;
    out.hookAng[0] = static_cast<int16_t>(-cM_atan2s(toLine.y, toLine.z));
    out.hookAng[1] = static_cast<int16_t>(
        cM_atan2s(toLine.x, std::sqrt(toLine.y * toLine.y + toLine.z * toLine.z)));
    out.hookAng[2] = actor->current.angle.z;
    const cXyz& bend = rod->mg_line.pos[70];
    out.bendTarget[0] = bend.x;
    out.bendTarget[1] = bend.y;
    out.bendTarget[2] = bend.z;
    out.extend = rod->field_0x6a4;
    out.bend = rod->field_0x6f8;
    const csXyz& a1 = link->mFishingArm1Angle;
    const csXyz& a2 = link->field_0x3160;
    const int16_t arms[6] = {a1.x, a1.y, a1.z, a2.x, a2.y, a2.z};
    std::copy(arms, arms + 3, out.arm1);
    std::copy(arms + 3, arms + 6, out.arm2);
    for (int i = 0; i < kFishingLinePoints; i++) {
        const cXyz& p = rod->mg_line.pos[kLineIndex[i]];
        out.line[i][0] = p.x;
        out.line[i][1] = p.y;
        out.line[i][2] = p.z;
    }
}

void encode(const RemoteFishing& f, int32_t out[kWireSize]) {
    std::fill(out, out + kWireSize, 0);
    if (!f.active) {
        return;
    }
    out[0] = 1 | (f.action << 1) | (f.hookKind << 4) | (f.esaKind << 5) | ((f.aimDown ? 1 : 0) << 7);
    putPos(out + 1, cXyz(f.bobber[0], f.bobber[1], f.bobber[2]));
    std::copy(f.bobberAng, f.bobberAng + 6, out + 4);
    putPos(out + 10, cXyz(f.hook[0], f.hook[1], f.hook[2]));
    std::copy(f.hookAng, f.hookAng + 3, out + 13);
    putPos(out + 16, cXyz(f.bendTarget[0], f.bendTarget[1], f.bendTarget[2]));
    out[19] = wireRatio(f.extend);
    out[20] = wireRatio(f.bend / 100.0f);
    std::copy(f.arm1, f.arm1 + 3, out + 21);
    std::copy(f.arm2, f.arm2 + 3, out + 24);
    for (int i = 0; i < kFishingLinePoints; i++) {
        putPos(out + 27 + 3 * i, cXyz(f.line[i][0], f.line[i][1], f.line[i][2]));
    }
}

RemoteFishing decode(const int32_t in[kWireSize]) {
    RemoteFishing f;
    if ((in[0] & 1) == 0) {
        return f;
    }
    f.active = true;
    f.action = static_cast<uint8_t>((in[0] >> 1) & 7);
    f.hookKind = static_cast<uint8_t>((in[0] >> 4) & 1);
    f.esaKind = static_cast<uint8_t>(std::min((in[0] >> 5) & 3, 2));
    f.aimDown = ((in[0] >> 7) & 1) != 0;
    takePos(in + 1, f.bobber);
    for (int i = 0; i < 6; i++) {
        f.bobberAng[i] = static_cast<int16_t>(in[4 + i]);
    }
    takePos(in + 10, f.hook);
    for (int i = 0; i < 3; i++) {
        f.hookAng[i] = static_cast<int16_t>(in[13 + i]);
    }
    takePos(in + 16, f.bendTarget);
    f.extend = std::clamp(in[19] / kRatioScale, 0.0f, 1.5f);
    f.bend = std::clamp(in[20] / kRatioScale * 100.0f, 0.0f, 400.0f);
    for (int i = 0; i < 3; i++) {
        f.arm1[i] = static_cast<int16_t>(in[21 + i]);
        f.arm2[i] = static_cast<int16_t>(in[24 + i]);
    }
    for (int i = 0; i < kFishingLinePoints; i++) {
        takePos(in + 27 + 3 * i, f.line[i]);
    }
    // Everything hangs off the rod: a sample far from the tip is junk.
    for (int i = 1; i < kFishingLinePoints; i++) {
        const float dx = f.line[i][0] - f.line[0][0];
        const float dy = f.line[i][1] - f.line[0][1];
        const float dz = f.line[i][2] - f.line[0][2];
        if (!(dx * dx + dy * dy + dz * dz <= kMaxSpan * kMaxSpan)) {
            return RemoteFishing{};
        }
    }
    return f;
}

}  // namespace twili::fishing
