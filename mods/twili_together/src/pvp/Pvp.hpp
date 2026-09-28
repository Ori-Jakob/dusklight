#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct cXyz;
class daAlink_c;
class dCcD_GObjInf;
class fopAc_ac_c;

// PvP (room setting pvpMode): types shared with the dummy; the system itself arrives in P5.
namespace twili {

struct Client;

namespace pvp {

enum class Kind : uint8_t {
    Sword,
    Wolf,
    Arrow,
    Bomb,
    Slingshot,
    Boomerang,
    Hookshot,
    ShieldBash,
    IronBall,
    Stomp,
    Count,
};

// On the wire as DAMAGE_PLAYER "spl".
enum class Knockback : uint8_t {
    Light,
    Knockdown,
    Count,
};

// Quarter hearts per hit; the server clamps to it too.
inline constexpr int kMaxHitDamage = 4;

struct HitReport {
    Kind kind = Kind::Sword;
    uint8_t damage = 0;  // quarter hearts; 0 staggers only
    Knockback knockback = Knockback::Light;
    int16_t dirY = 0;
    bool blocked = false;
    // The victim's PLAYER_UPDATE seq the attacker's dummy showed.
    uint32_t viewSeq = 0;
};

struct PendingHit {
    uint32_t attackerId = 0;
    uint32_t hitId = 0;
    HitReport hit;
    std::chrono::steady_clock::time_point arrivedAt{};
};

// Whether `atActor`'s collider `at` is one of our weapons, and the hit it deals the dummy.
bool classifyLocalAttack(fopAc_ac_c* atActor, dCcD_GObjInf* at, dCcD_GObjInf* tg,
    fopAc_ac_c* dummy, HitReport& out);
// Whether `client`'s dummy registers its hurtbox this frame.
bool hurtboxEnabled(const Client& client);
// Our bomb arrow exploded at `pos`: that NBOMB counts as ours for a moment.
void noteLocalExplosion(const cXyz& pos);
// Our PLAYER_UPDATE kVisGuard / kVisPvpImmune bits.
uint16_t localVisFlags(daAlink_c* link);

}  // namespace pvp
}  // namespace twili
