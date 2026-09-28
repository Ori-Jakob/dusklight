// Which of our colliders count as weapons against a remote player's dummy and what they deal.

#include "pvp/Pvp.hpp"

#include "core/Client.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_nbomb.h"
#include "d/d_cc_d.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <iterator>

namespace twili::pvp {
namespace {

// Vanilla attack powers are tuned for enemies; PvP has its own table, never above kMaxHitDamage.
// The Master Sword adds kMasterSwordBonus to a sword row, the wooden sword takes one off.
enum class Attack : uint8_t {
    Slash,       // normal, dash, stab and horseback cuts
    Finisher,    // combo finishers, back slice, mortal draw
    JumpStrike,  // jump attack, helm splitter, ending blow
    Spin,
    GreatSpin,
    Wolf,       // bites, pounces, the wolf's spin
    MidnaLock,  // Midna's area attack
    IronBall,
    Stomp,  // iron boots
    Hookshot,
    ShieldBash,
    Arrow,
    LightArrow,
    Bomb,  // bombs and bomb arrows
    Boomerang,
    Slingshot,
    Count,
};

struct Row {
    Kind kind;
    uint8_t damage;
    Knockback knockback;
};

constexpr Row kTable[] = {
    {Kind::Sword, 2, Knockback::Light},         // Slash
    {Kind::Sword, 3, Knockback::Light},         // Finisher
    {Kind::Sword, 3, Knockback::Knockdown},     // JumpStrike
    {Kind::Sword, 3, Knockback::Light},         // Spin
    {Kind::Sword, 4, Knockback::Knockdown},     // GreatSpin
    {Kind::Wolf, 2, Knockback::Light},          // Wolf
    {Kind::Wolf, 2, Knockback::Light},          // MidnaLock
    {Kind::IronBall, 4, Knockback::Knockdown},  // IronBall
    {Kind::Stomp, 2, Knockback::Light},         // Stomp
    {Kind::Hookshot, 0, Knockback::Light},      // Hookshot: stagger only
    {Kind::ShieldBash, 0, Knockback::Light},    // ShieldBash: stagger only
    {Kind::Arrow, 2, Knockback::Light},         // Arrow
    {Kind::Arrow, 3, Knockback::Light},         // LightArrow
    {Kind::Bomb, 3, Knockback::Knockdown},      // Bomb
    {Kind::Boomerang, 0, Knockback::Light},     // Boomerang: stagger only
    {Kind::Slingshot, 0, Knockback::Light},     // Slingshot: stagger only
};
static_assert(std::size(kTable) == static_cast<size_t>(Attack::Count));
constexpr int kMasterSwordBonus = 1;

constexpr bool tableWithinCap() {
    for (const Row& row : kTable) {
        if (row.damage > kMaxHitDamage) {
            return false;
        }
    }
    return true;
}
static_assert(tableWithinCap());

constexpr const char* kKindNames[] = {"sword", "wolf", "arrow", "bomb", "slingshot", "boomerang",
    "hookshot", "shieldBash", "ironBall", "stomp"};
static_assert(std::size(kKindNames) == static_cast<size_t>(Kind::Count));

constexpr const char* kKnockbackNames[] = {"light", "knockdown"};
static_assert(std::size(kKnockbackNames) == static_cast<size_t>(Knockback::Count));

Attack swordAttack(int cutType) {
    switch (cutType) {
    case daPy_py_c::CUT_TYPE_LARGE_TURN_LEFT:
    case daPy_py_c::CUT_TYPE_LARGE_TURN_RIGHT:
        return Attack::GreatSpin;
    case daPy_py_c::CUT_TYPE_TURN_LEFT:
    case daPy_py_c::CUT_TYPE_TURN_RIGHT:
        return Attack::Spin;
    case daPy_py_c::CUT_TYPE_JUMP:
    case daPy_py_c::CUT_TYPE_HEAD_JUMP:
    case daPy_py_c::CUT_TYPE_LARGE_JUMP_INIT:
    case daPy_py_c::CUT_TYPE_LARGE_JUMP:
    case daPy_py_c::CUT_TYPE_LARGE_JUMP_FINISH:
    case daPy_py_c::CUT_TYPE_DOWN:
        return Attack::JumpStrike;
    case daPy_py_c::CUT_TYPE_FINISH_LEFT:
    case daPy_py_c::CUT_TYPE_FINISH_RIGHT:
    case daPy_py_c::CUT_TYPE_FINISH_VERTICAL:
    case daPy_py_c::CUT_TYPE_FINISH_STAB:
    case daPy_py_c::CUT_TYPE_TWIRL:
    case daPy_py_c::CUT_TYPE_MORTAL_DRAW_A:
    case daPy_py_c::CUT_TYPE_MORTAL_DRAW_B:
        return Attack::Finisher;
    default:
        return Attack::Slash;
    }
}

// Our Link's own colliders: sword, wolf body and head, Ball and Chain, stomps, hookshot, bashes.
bool linkAttack(daAlink_c* link, dCcD_GObjInf* at, Attack& out) {
    const u32 type = at->GetAtType();
    if (type & AT_TYPE_MIDNA_LOCK) {
        out = Attack::MidnaLock;
    } else if (type & (AT_TYPE_WOLF_ATTACK | AT_TYPE_WOLF_CUT_TURN)) {
        out = Attack::Wolf;
    } else if (type & (AT_TYPE_NORMAL_SWORD | AT_TYPE_MASTER_SWORD)) {
        out = swordAttack(link->getCutType());
    } else if (type & AT_TYPE_IRON_BALL) {
        out = Attack::IronBall;
    } else if (type & AT_TYPE_HEAVY_BOOTS) {
        out = Attack::Stomp;
    } else if (type & AT_TYPE_HOOKSHOT) {
        out = Attack::Hookshot;
    } else if (type & AT_TYPE_SHIELD_ATTACK) {
        out = Attack::ShieldBash;
    } else {
        return false;
    }
    return true;
}

// Our bomb arrows' blasts: that NBOMB is created at the noted position and hits for 3 ticks.
struct NotedExplosion {
    cXyz pos;
    uint32_t seq = 0;  // 0 for an empty entry
};
constexpr int kNotedExplosions = 4;
constexpr uint32_t kNotedExplosionTicks = 10;
NotedExplosion s_explosions[kNotedExplosions];
int s_nextExplosion = 0;

bool isNotedExplosion(const cXyz& home) {
    const uint32_t now = Session::localPoseSeq();
    for (const NotedExplosion& e : s_explosions) {
        if (e.seq != 0 && now - e.seq <= kNotedExplosionTicks && e.pos.abs2(home) <= 1.0f) {
            return true;
        }
    }
    return false;
}

// Every arrow counts (enemies shoot d_a_e_arrow); a bomb if we made it or our arrow blew up.
bool projectileAttack(fopAc_ac_c* atActor, dCcD_GObjInf* at, Attack& out) {
    switch (fopAcM_GetName(atActor)) {
    case fpcNm_ARROW_e:
        // A bomb arrow deals nothing: counting it would start the cooldown that drops its blast.
        if (static_cast<daArrow_c*>(atActor)->checkBombArrow()) {
            return false;
        }
        if (at->GetAtType() & AT_TYPE_SLINGSHOT) {
            out = Attack::Slingshot;
        } else if (at->GetAtMtrl() == dCcD_MTRL_LIGHT) {
            out = Attack::LightArrow;
        } else {
            out = Attack::Arrow;
        }
        return true;
    case fpcNm_BOOMERANG_e:
        // In hand its capsule is the aim's lock line: only the thrown one hits.
        if (fopAcM_GetParam(atActor) == 0) {
            return false;
        }
        out = Attack::Boomerang;
        return true;
    case fpcNm_NBOMB_e: {
        auto* bomb = static_cast<daNbomb_c*>(atActor);
        if (!bomb->checkPlayerMake() && !isNotedExplosion(bomb->home.pos)) {
            return false;
        }
        out = Attack::Bomb;
        return true;
    }
    default:
        return false;
    }
}

void fillFromTable(Attack attack, HitReport& out) {
    const Row& row = kTable[static_cast<size_t>(attack)];
    int damage = row.damage;
    if (row.kind == Kind::Sword && damage > 0) {
        if (daPy_py_c::checkMasterSwordEquip()) {
            damage += kMasterSwordBonus;
        } else if (daPy_py_c::checkWoodSwordEquip()) {
            damage = std::max(damage - 1, 1);
        }
    }
    out.kind = row.kind;
    out.damage = static_cast<uint8_t>(std::min(damage, kMaxHitDamage));
    out.knockback = row.knockback;
}

}  // namespace

const char* kindName(Kind kind) {
    const size_t i = static_cast<size_t>(kind);
    return i < std::size(kKindNames) ? kKindNames[i] : "?";
}

bool kindFromName(const std::string& name, Kind& out) {
    for (size_t i = 0; i < std::size(kKindNames); i++) {
        if (name == kKindNames[i]) {
            out = static_cast<Kind>(i);
            return true;
        }
    }
    return false;
}

const char* knockbackName(Knockback knockback) {
    const size_t i = static_cast<size_t>(knockback);
    return i < std::size(kKnockbackNames) ? kKnockbackNames[i] : "?";
}

bool knockbackFromName(const std::string& name, Knockback& out) {
    for (size_t i = 0; i < std::size(kKnockbackNames); i++) {
        if (name == kKnockbackNames[i]) {
            out = static_cast<Knockback>(i);
            return true;
        }
    }
    return false;
}

// A modified attacker cannot make a stagger-only weapon hurt or a wolf bite knock down.
void clampToTable(HitReport& hit) {
    int maxDamage = 0;
    bool light = false;
    bool knockdown = false;
    for (const Row& row : kTable) {
        if (row.kind != hit.kind) {
            continue;
        }
        const int bonus = row.kind == Kind::Sword && row.damage > 0 ? kMasterSwordBonus : 0;
        maxDamage = std::max(maxDamage, std::min(row.damage + bonus, kMaxHitDamage));
        (row.knockback == Knockback::Knockdown ? knockdown : light) = true;
    }
    hit.damage = static_cast<uint8_t>(std::min<int>(hit.damage, maxDamage));
    if (hit.knockback == Knockback::Knockdown && !knockdown) {
        hit.knockback = Knockback::Light;
    } else if (hit.knockback == Knockback::Light && !light) {
        hit.knockback = Knockback::Knockdown;
    }
}

// Called from the dummy's TG hit callback inside the collision pass, while a projectile lives.
bool classifyLocalAttack(
    fopAc_ac_c* atActor, dCcD_GObjInf* at, dCcD_GObjInf* tg, fopAc_ac_c* dummy, HitReport& out) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (atActor == nullptr || at == nullptr || tg == nullptr || dummy == nullptr || link == nullptr)
    {
        return false;
    }
    // Enemy weapons and traps can reach the hurtbox too; only our own weapons count.
    Attack attack;
    if (atActor == link ? !linkAttack(link, at, attack) : !projectileAttack(atActor, at, attack)) {
        return false;
    }
    fillFromTable(attack, out);
    // As getDamageVec: the AT's motion, else from the attacker to the victim.
    cXyz vec = *tg->GetTgRVecP();
    if (vec.abs2XZ() < 0.1f) {
        vec = dummy->current.pos - atActor->current.pos;
    }
    out.dirY = vec.abs2XZ() < 0.1f ? atActor->shape_angle.y : vec.atan2sX_Z();
    out.blocked = tg->ChkTgShieldHit();
    return true;
}

void noteLocalExplosion(const cXyz& pos) {
    NotedExplosion& e = s_explosions[s_nextExplosion];
    s_nextExplosion = (s_nextExplosion + 1) % kNotedExplosions;
    e.pos = pos;
    e.seq = Session::localPoseSeq() != 0 ? Session::localPoseSeq() : 1;
}

bool hurtboxEnabled(const Client& client) {
    const Session& s = Session::instance();
    return s.isConnected() && s.roomState().pvpMode && isSaveLoaded() && client.isSaveLoaded &&
           pvpAllowedWith(client) && !(client.presenceFlags & kPresenceInCutscene) &&
           // The newest flags, not the delayed pose: the victim would drop the hit anyway.
           !(client.visFlags & kVisPvpImmune);
}

uint16_t localVisFlags(daAlink_c* link) {
    if (!Session::active() || !Session::instance().roomState().pvpMode) {
        return 0;
    }
    uint16_t flags = 0;
    if (link->mTgCyls[0].ChkTgShield() || link->mTgCyls[0].ChkTgSpShield()) {
        flags |= kVisGuard;
    }
    // The first two also turn our real TG off (setCollision).
    if (link->mDamageTimer != 0 || link->checkModeFlg(daAlink_c::MODE_HIT_STUN) ||
        dComIfGp_event_runCheck() || link->mProcID == daAlink_c::PROC_DEAD ||
        dComIfGs_getLife() == 0 || link->getClothesChangeWaitTimer() != 0)
    {
        flags |= kVisPvpImmune;
    }
    return flags;
}

}  // namespace twili::pvp
