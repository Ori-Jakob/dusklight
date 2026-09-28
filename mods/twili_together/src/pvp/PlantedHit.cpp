// Victim side: a received hit planted in mTgCyls[0] for one checkDamageAction.

#include "pvp/Pvp.hpp"

#include "core/GameAccess.hpp"
#include "core/Session.hpp"

#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_cc_d.h"
#include "d/d_com_inf_game.h"

#include <algorithm>
#include <iterator>

namespace twili::pvp {
namespace {

// Never AT_TYPE_BOMB: checkDamageAction replaces a bomb's damage with its own value.
struct KindLook {
    u32 atType;
    u8 se;  // setGuardSe and the hit sounds read it
};
constexpr KindLook kKindLooks[] = {
    {AT_TYPE_NORMAL_SWORD, dCcD_SE_SWORD},
    {(u32)AT_TYPE_WOLF_ATTACK, dCcD_SE_WOLF_BITE},
    {AT_TYPE_ARROW, dCcD_SE_ARROW_STICK},
    {AT_TYPE_0, dCcD_SE_NONE},  // bomb
    {AT_TYPE_SLINGSHOT, dCcD_SE_PACHINKO},
    {AT_TYPE_BOOMERANG, dCcD_SE_WOOD},
    {AT_TYPE_HOOKSHOT, dCcD_SE_HOOKSHOT_STICK},
    {AT_TYPE_SHIELD_ATTACK, dCcD_SE_SHIELD_ATTACK},
    {AT_TYPE_IRON_BALL, dCcD_SE_HAMMER},
    {AT_TYPE_HEAVY_BOOTS, dCcD_SE_HAMMER},
};
static_assert(std::size(kKindLooks) == static_cast<size_t>(Kind::Count));

// The AT spl checkDamageAction turns into each knockback: the small stagger or a knockdown.
dCcG_At_Spl vanillaSpl(Knockback knockback) {
    return knockback == Knockback::Knockdown ? dCcG_At_Spl_UNK_1 : dCcG_At_Spl_UNK_0;
}

// The attacker the planted hit points at; never registered with the collision system.
dCcD_Cyl& scratchAt() {
    static dCcD_Cyl s_at;
    static const bool s_set = [] {
        const dCcD_SrcCyl src = {};
        s_at.Set(src);
        return true;
    }();
    (void)s_set;
    return s_at;
}

enum class Taken : uint8_t { No, Applied, Blocked };

struct Plant {
    daAlink_c* link = nullptr;
    PendingHit pending;
    bool blocked = false;
    // The TG branch read our hit (the poly-damage branch would call setDamagePoint first).
    bool entered = false;
    Taken taken = Taken::No;
    int lifeTaken = 0;
};
Plant s_plant;
// The warm GetTgHitGObj tap tests only this.
bool s_planted = false;

// dCcS::ChkShieldFrontRange for a target without joint collision.
bool shieldFaces(daAlink_c* link, int16_t dirY, bool halfRange) {
    int range = 0x4000;
    if (halfRange) {
        range >>= 1;
    }
    return cLib_distanceAngleS(static_cast<s16>(dirY - -0x8000), link->field_0x306c) <= range;
}

// damageMagnification without the Dusk difficulty options, rounded up like setDamagePoint.
int victimDamage(daAlink_c* link, int dmg) {
    if (dmg <= 0) {
        return 0;
    }
    f32 mag = 1.0f;
    if (link->checkNoResetFlg3(daPy_py_c::FLG3_UNK_40000000) &&
        !link->checkEndResetFlg2(daPy_py_c::ERFLG2_UNK_40))
    {
        mag = 1.5f;
    }
    if (link->checkWolf() && !link->checkCargoCarry()) {
        mag *= 2.0f;
    }
    const f32 f = static_cast<f32>(dmg) * mag;
    int out = static_cast<int>(f);
    if (static_cast<int>(f * 10.0f) % 10 != 0) {
        out++;
    }
    return std::min(out, kMaxHitDamage);
}

// Without pvpLethal a hit never takes life below one heart (or the current life if less).
int clampToFloor(daAlink_c* link, int dmg) {
    if (Session::instance().roomState().pvpLethal || link->checkMagicArmorNoDamage()) {
        return dmg;
    }
    const int life = dComIfGs_getLife() + static_cast<int>(dComIfGp_getItemLifeCount());
    const int floor = std::min(life, 4);
    return std::clamp(life - floor, 0, dmg);
}

// armorRupeeDrain INVINCIBLE is the one mode whose armor stops damage without rupees.
bool magicArmorInvincible(daAlink_c* link) {
    const u16 rupees = dComIfGs_getRupee();
    dComIfGs_setRupee(0);
    const bool invincible = link->checkMagicArmorNoDamage();
    dComIfGs_setRupee(rupees);
    return invincible;
}

// setDamagePoint for an already magnified value; the life taken (magic armor drains rupees).
int applyDamage(daAlink_c* link, int dmg) {
    if (link->checkWolf()) {
        link->offWolfEyeUp();
    }
    int lifeTaken = 0;
    if (link->checkMagicArmorNoDamage()) {
        if (!magicArmorInvincible(link)) {
            dComIfGp_setItemRupeeCount(-dmg * 10);
        }
    } else {
        dComIfGp_setItemLifeCount(static_cast<f32>(-dmg), 0);
        lifeTaken = dmg;
    }
    link->onResetFlg1(daPy_py_c::RFLG1_DAMAGE_IMPACT);
    link->mSwordUpTimer = 0;
    link->mDamageTimer = link->mpHIO->mDamage.m.mInvincibleTime;
    link->setDamageColorTime();
    return lifeTaken;
}

bool realTgHit(daAlink_c* link) {
    for (dCcD_Cyl& cyl : link->mTgCyls) {
        if (cyl.ChkTgHit()) {
            return true;
        }
    }
    return link->checkWolf() && link->mAtSph.ChkTgHit();
}

void plant(daAlink_c* link, const PendingHit& pending) {
    const HitReport& hit = pending.hit;
    dCcD_Cyl& own = link->mTgCyls[0];
    // Defender-favoured; the shield flags are from our last setCollision.
    const bool shield = own.ChkTgShield();
    const bool spShield = own.ChkTgSpShield();
    const bool blocked =
        hit.blocked || ((shield || spShield) &&
                           (!own.ChkTgShieldFrontRange() || shieldFaces(link, hit.dirY, !shield)));

    const KindLook& look =
        kKindLooks[std::min(static_cast<size_t>(hit.kind), std::size(kKindLooks) - 1)];
    dCcD_Cyl& at = scratchAt();
    at.SetAtType(look.atType);
    at.SetAtSe(look.se);
    at.SetAtSpl(vanillaSpl(hit.knockback));
    at.SetAtMtrl(dCcD_MTRL_NONE);
    at.SetAtAtp(0);

    own.ResetTgHit();
    own.SetTgHit(&at);
    // ChkTgHit / GetTgHitObj without a live attacker actor.
    own.OnTgHitNoActor();
    cXyz rvec(cM_ssin(hit.dirY) * 10.0f, 0.0f, cM_scos(hit.dirY) * 10.0f);
    own.SetTgRVec(rvec);
    cXyz pos(link->current.pos.x, link->current.pos.y + 100.0f, link->current.pos.z);
    own.SetTgHitPos(pos);
    if (blocked) {
        own.OnTgShieldHit();
    }
    // The vanilla damage read is 0; our own damage goes in at setDamagePoint.
    link->mCcStts.ClrTg();

    s_plant = Plant{};
    s_plant.link = link;
    s_plant.pending = pending;
    s_plant.blocked = blocked;
    s_planted = true;
}

}  // namespace

void beginDamageCheck(daAlink_c* link) {
    if (s_planted || link == nullptr || link != localLink() || !Session::active()) {
        return;
    }
    // The same condition that turns our TG off in setCollision; a real hit wins.
    if (link->mDamageTimer != 0 || link->checkModeFlg(daAlink_c::MODE_HIT_STUN) ||
        link->checkEventRun() || realTgHit(link))
    {
        return;
    }
    PendingHit pending;
    if (takePendingHit(pending)) {
        plant(link, pending);
    }
}

void onTgBranchEntered(const dCcD_GObjInf* tg) {
    if (s_planted && tg == &s_plant.link->mTgCyls[0]) {
        s_plant.entered = true;
    }
}

void onGuardSe(daAlink_c* link, const dCcD_GObjInf* tg) {
    if (s_planted && s_plant.entered && link == s_plant.link && tg == &link->mTgCyls[0] &&
        s_plant.taken == Taken::No)
    {
        s_plant.taken = Taken::Blocked;
    }
}

void onDamagePoint(daAlink_c* link) {
    if (!s_planted || !s_plant.entered || link != s_plant.link || s_plant.taken != Taken::No) {
        return;
    }
    s_plant.taken = Taken::Applied;
    const int dmg = clampToFloor(link, victimDamage(link, s_plant.pending.hit.damage));
    if (dmg > 0) {
        s_plant.lifeTaken = applyDamage(link, dmg);
    } else {
        // A pure stagger or a hit the floor ate: i-frames all the same, so hits cannot chain.
        link->mDamageTimer = link->mpHIO->mDamage.m.mInvincibleTime;
    }
}

void endDamageCheck(daAlink_c* link) {
    if (!s_planted || link != s_plant.link) {
        return;
    }
    s_planted = false;
    // No pointer to our scratch collider survives the call.
    link->mTgCyls[0].ResetTgHit();
    const Plant done = s_plant;
    s_plant = Plant{};
    if (done.taken == Taken::No) {
        requeueHit(done.pending);
        return;
    }
    // Taken as a block only if the guard branch got it first.
    const bool blocked = done.taken == Taken::Blocked;
    reportResult(done.pending, blocked ? "blocked" : "applied", "", blocked ? 0 : done.lifeTaken);
}

}  // namespace twili::pvp
