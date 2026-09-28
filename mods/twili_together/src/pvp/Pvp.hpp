#pragma once

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstdint>
#include <string>

struct cXyz;
class daAlink_c;
class dCcD_GObjInf;
class fopAc_ac_c;

// PvP: the attacker's game decides a hit, the victim plants it on its own Link (PlantedHit.cpp).
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

// What the autotest reads.
struct AttackStats {
    uint32_t sent = 0, applied = 0, blocked = 0, dropped = 0, refused = 0;
    int damage = 0;
    uint32_t lastHitId = 0;
    bool lastAnswered = false;
    std::string lastResult, lastReason;
    int lastDamage = 0;
};
struct VictimStats {
    uint32_t taken = 0, blocked = 0, dropped = 0;  // taken counts applied and blocked
    int damage = 0;
    std::string lastDropReason;
};

const char* kindName(Kind kind);
const char* knockbackName(Knockback knockback);
bool kindFromName(const std::string& name, Kind& out);
bool knockbackFromName(const std::string& name, Knockback& out);

// Attacker side.
// Whether `atActor`'s collider `at` is one of our weapons, and the hit it deals the dummy.
bool classifyLocalAttack(
    fopAc_ac_c* atActor, dCcD_GObjInf* at, dCcD_GObjInf* tg, fopAc_ac_c* dummy, HitReport& out);
// Whether `client`'s dummy registers its hurtbox this frame.
bool hurtboxEnabled(const Client& client);
// Our bomb arrow exploded at `pos`: that NBOMB counts as ours for a moment.
void noteLocalExplosion(const cXyz& pos);

// Victim side.
// At most what the PvP table allows for the hit's kind.
void clampToTable(HitReport& hit);
// Our PLAYER_UPDATE kVisGuard / kVisPvpImmune bits.
uint16_t localVisFlags(daAlink_c* link);
// Plants a pending hit into `link`'s TG before its damage check, restores it after (hooks).
void beginDamageCheck(daAlink_c* link);
void onTgBranchEntered(const dCcD_GObjInf* tg);
void onGuardSe(daAlink_c* link, const dCcD_GObjInf* tg);
void onDamagePoint(daAlink_c* link);
void endDamageCheck(daAlink_c* link);

// Session side (DamagePlayer.cpp).
bool pvpAllowedWith(const Client& client);
void queueHit(uint32_t victimId, const HitReport& hit);
bool takePendingHit(PendingHit& out);
// A hit the damage check did not reach goes back to the front of the queue.
void requeueHit(const PendingHit& hit);
void reportResult(const PendingHit& hit, const char* result, const char* reason, int damage);
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// After the dummies: sends what the last collision pass registered, expires waiting hits.
void tick();
void resetSession();
uint32_t sendDamagePlayerForTest(uint32_t victimId, const HitReport& hit);
const AttackStats* attackStats(uint32_t victimId);
const VictimStats& victimStats();

}  // namespace pvp
}  // namespace twili
