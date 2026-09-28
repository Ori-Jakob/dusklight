#include "fx/StatusFx.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"

#include <algorithm>
#include <cmath>

namespace twili::statusfx {
namespace {

// The local player on the previous tick, for the one-shot generations.
struct SenderState {
    fpc_ProcID player = fpcM_ERROR_PROCESS_ID_e;
    uint8_t prevShieldBurn = 0;  // field_0x2fcb
    uint8_t prevShield = dItemNo_NONE_e;
    bool fireActive[4] = {};
    uint32_t fireEmitter[4] = {};  // firePointEff_c::field_0x4
    // Kept across player actors
    uint8_t burnOutSeq = 0;
    uint8_t fireGen[4] = {};
};

SenderState s_tx;
RemoteStatusFx s_last;

// setFreezeEffect's gate for the ice block (d_a_alink_effect.inc).
bool iceBlockProc(const daAlink_c* link) {
    return link->mProcID == daAlink_c::PROC_DAMAGE ||
           link->mProcID == daAlink_c::PROC_SWIM_FREEZE_RETURN ||
           link->mProcID == daAlink_c::PROC_WOLF_DAMAGE;
}

int8_t wholeUnits(f32 v) {
    if (!std::isfinite(v)) {
        return 0;
    }
    return static_cast<int8_t>(std::clamp<long>(std::lround(v), -127, 127));
}

}  // namespace

void captureLocal(RemoteStatusFx& out) {
    out = RemoteStatusFx{};
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        s_last = out;
        return;
    }
    const fpc_ProcID id = fopAcM_GetID(link);
    const uint8_t shield = dComIfGs_getSelectEquipShield();
    if (id != s_tx.player) {
        // A new player actor
        s_tx.player = id;
        s_tx.prevShieldBurn = 0;
        s_tx.prevShield = shield;
        std::fill(std::begin(s_tx.fireActive), std::end(s_tx.fireActive), false);
        std::fill(std::begin(s_tx.fireEmitter), std::end(s_tx.fireEmitter), 0u);
    }

    if (s_tx.prevShieldBurn != 0 && link->field_0x2fcb == 0 && shield == dItemNo_NONE_e &&
        s_tx.prevShield != dItemNo_NONE_e)
    {
        s_tx.burnOutSeq = (s_tx.burnOutSeq + 1) & 7;
    }
    s_tx.prevShieldBurn = link->field_0x2fcb;
    s_tx.prevShield = shield;

    if (link->checkFreezeDamage()) {
        out.flags |= kStatusFrozen;
        if (iceBlockProc(link)) {
            out.flags |= kStatusIceBlock;
        }
    }
    if (link->mProcID == daAlink_c::PROC_ELEC_DAMAGE) {
        out.flags |= kStatusElec;
    }
    // The BRK execute binds when the rupees run out (setMagicArmorBrk(0)), not the rupee count
    if (link->checkMagicArmorWearAbility() && link->field_0x2fd7 == 0) {
        out.flags |= kStatusArmorDrained;
    }
    out.shieldBurnOutSeq = s_tx.burnOutSeq;
    out.shieldBurn = daPy_py_c::checkWoodShieldEquip() ? link->field_0x2fcb : 0;
    if (link->mDamageTimer > 0 && !link->checkMagicArmorNoDamage()) {
        out.damageTimer = static_cast<uint8_t>(std::min<int>(link->mDamageTimer, 255));
        out.damageColorTime = link->mDamageColorTime;
    }
    out.iceWait = static_cast<uint8_t>(std::clamp<int>(link->mIceDamageWaitTimer, 0, 255));
    out.sinkOffset = std::isfinite(link->mSinkShapeOffset)
                         ? std::clamp(link->mSinkShapeOffset, -256.0f, 256.0f)
                         : 0.0f;

    for (int i = 0; i < 4; i++) {
        const daAlink_c::firePointEff_c& e = link->field_0x32d8[i];
        const bool active = e.field_0x0 != 0;
        // (Re)ignition
        if (active && (!s_tx.fireActive[i] ||
                       (s_tx.fireEmitter[i] != 0 && e.field_0x4 != s_tx.fireEmitter[i])))
        {
            s_tx.fireGen[i] = (s_tx.fireGen[i] + 1) & 7;
        }
        s_tx.fireActive[i] = active;
        s_tx.fireEmitter[i] = active ? e.field_0x4 : 0;
        if (active && e.field_0x2 < 64) {
            RemoteFirePoint& f = out.fire[i];
            f.joint = static_cast<uint8_t>(e.field_0x2);
            f.gen = s_tx.fireGen[i];
            f.off[0] = wholeUnits(e.field_0x18.x);
            f.off[1] = wholeUnits(e.field_0x18.y);
            f.off[2] = wholeUnits(e.field_0x18.z);
        }
    }
    s_last = out;
}

const RemoteStatusFx& lastCaptured() {
    return s_last;
}

void resetSender() {
    s_tx = SenderState{};
    s_last = RemoteStatusFx{};
}

void encode(const RemoteStatusFx& s, int32_t sx[5], int32_t sf[8]) {
    sx[0] = (s.flags & kStatusWireMask) | ((s.shieldBurnOutSeq & 7) << 8);
    sx[1] = s.shieldBurn;
    sx[2] = s.damageTimer | (s.damageColorTime << 8);
    sx[3] = s.iceWait;
    sx[4] = std::isfinite(s.sinkOffset) ? static_cast<int32_t>(std::lround(s.sinkOffset * kPosScale))
                                        : 0;
    for (int i = 0; i < 4; i++) {
        const RemoteFirePoint& f = s.fire[i];
        if (!f.active()) {
            sf[2 * i] = sf[2 * i + 1] = 0;
            continue;
        }
        sf[2 * i] = 1 | ((f.joint & 0x3F) << 1) | ((f.gen & 7) << 7);
        sf[2 * i + 1] = static_cast<uint8_t>(f.off[0]) | (static_cast<uint8_t>(f.off[1]) << 8) |
                        (static_cast<uint8_t>(f.off[2]) << 16);
    }
}

RemoteStatusFx decode(const int32_t sx[5], const int32_t sf[8]) {
    RemoteStatusFx s;
    const uint32_t head = static_cast<uint32_t>(sx[0]);
    s.flags = static_cast<uint8_t>(head & kStatusWireMask);
    s.shieldBurnOutSeq = static_cast<uint8_t>((head >> 8) & 7);
    s.shieldBurn = static_cast<uint8_t>(std::clamp<int32_t>(sx[1], 0, 120));
    s.damageTimer = static_cast<uint8_t>(sx[2] & 0xFF);
    s.damageColorTime = static_cast<uint8_t>((sx[2] >> 8) & 0xFF);
    s.iceWait = static_cast<uint8_t>(std::clamp<int32_t>(sx[3], 0, 255));
    const int32_t sinkLimit = static_cast<int32_t>(256.0f * kPosScale);
    s.sinkOffset = std::clamp<int32_t>(sx[4], -sinkLimit, sinkLimit) / kPosScale;
    for (int i = 0; i < 4; i++) {
        const uint32_t h = static_cast<uint32_t>(sf[2 * i]);
        if ((h & 1) == 0) {
            continue;
        }
        RemoteFirePoint& f = s.fire[i];
        f.joint = static_cast<uint8_t>((h >> 1) & 0x3F);
        f.gen = static_cast<uint8_t>((h >> 7) & 7);
        const uint32_t o = static_cast<uint32_t>(sf[2 * i + 1]);
        for (int k = 0; k < 3; k++) {
            f.off[k] = static_cast<int8_t>((o >> (8 * k)) & 0xFF);
        }
    }
    return s;
}

}  // namespace twili::statusfx

