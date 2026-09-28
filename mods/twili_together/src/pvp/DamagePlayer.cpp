// DAMAGE_PLAYER / DAMAGE_RESULT. The server forwards a hit only while pvpMode is on and the two
// may fight, rebuilt with clamped values and stamped with the attacker's stage and layer; the
// victim checks it again and queues it for its next damage check. Neither packet is cached.

#include "pvp/Pvp.hpp"

#include "core/Client.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

namespace twili::pvp {
namespace {

using Clock = std::chrono::steady_clock;

// The victim has i-frames after a hit anyway; also clear of the server's 200 ms pair limit.
constexpr auto kSendCooldown = std::chrono::milliseconds(400);
// How long a received hit waits for our damage check (events, i-frames, menus).
constexpr auto kPendingTtl = std::chrono::milliseconds(300);
constexpr size_t kMaxPending = 4;
// The attacker may have hit a pose of ours at most this many ticks old.
constexpr int32_t kMaxViewAgeTicks = 30;

struct State {
    // Collision pass -> tick: the strongest hit per victim.
    std::map<uint32_t, HitReport> outgoing;
    std::map<uint32_t, Clock::time_point> lastSentAt;
    std::vector<PendingHit> pending;
    std::map<uint32_t, AttackStats> attacks;
    VictimStats victim;
    uint32_t nextHitId = 1;
};
State s_state;

// Melee reach plus lag, the blast radius, a projectile's flight.
float maxRange(Kind kind) {
    switch (kind) {
    case Kind::Arrow:
    case Kind::Slingshot:
    case Kind::Bomb:
        return 6000.0f;
    case Kind::Boomerang:
    case Kind::Hookshot:
        return 3000.0f;
    default:
        return 700.0f;
    }
}

int64_t clampedInt(const nlohmann::json& j, const char* key, int64_t lo, int64_t hi) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        return std::clamp<int64_t>(0, lo, hi);
    }
    const int64_t v = it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX) ?
                          INT64_MAX :
                          it->get<int64_t>();
    return std::clamp(v, lo, hi);
}

std::string shortString(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>().substr(0, 32);
}

const Client* findClient(uint32_t id) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(id);
    return it == clients.end() ? nullptr : &it->second;
}

uint32_t sendDamagePlayer(uint32_t victimId, const HitReport& hit) {
    const uint32_t hitId = s_state.nextHitId;
    s_state.nextHitId = s_state.nextHitId >= 0x7FFFFFFF ? 1 : s_state.nextHitId + 1;
    Session::instance().send({
        {"type", "DAMAGE_PLAYER"},
        {"targetClientId", victimId},
        {"hitId", hitId},
        {"kind", static_cast<int>(hit.kind)},
        {"damage", hit.damage},
        {"spl", static_cast<int>(hit.knockback)},
        {"dirY", hit.dirY},
        {"blocked", hit.blocked},
        {"viewSeq", hit.viewSeq},
    });
    AttackStats& st = s_state.attacks[victimId];
    st.sent++;
    st.lastHitId = hitId;
    st.lastAnswered = false;
    st.lastResult.clear();
    st.lastReason.clear();
    st.lastDamage = 0;
    TwiliLog.info("[pvp] hit {} -> client {} kind {} dmg {} {} blocked {}", hitId, victimId,
        kindName(hit.kind), hit.damage, knockbackName(hit.knockback), hit.blocked);
    return hitId;
}

void dropPending(const PendingHit& hit, const char* reason) {
    reportResult(hit, "dropped", reason, 0);
}

void handleDamagePlayer(const nlohmann::json& packet) {
    Session& session = Session::instance();
    const uint32_t self = session.selfClientId();
    const uint32_t from = static_cast<uint32_t>(clampedInt(packet, "clientId", 0, 0x7FFFFFFF));
    if (clampedInt(packet, "targetClientId", 0, 0x7FFFFFFF) != self || from == 0 || from == self) {
        return;
    }
    PendingHit p;
    p.attackerId = from;
    p.hitId = static_cast<uint32_t>(clampedInt(packet, "hitId", 0, 0x7FFFFFFF));
    p.hit.kind =
        static_cast<Kind>(clampedInt(packet, "kind", 0, static_cast<int64_t>(Kind::Count) - 1));
    p.hit.damage = static_cast<uint8_t>(clampedInt(packet, "damage", 0, kMaxHitDamage));
    // Anything but a defined knockback is the mildest one.
    const int64_t knockbackCount = static_cast<int64_t>(Knockback::Count);
    const int64_t spl = clampedInt(packet, "spl", -1, knockbackCount);
    p.hit.knockback =
        spl >= 0 && spl < knockbackCount ? static_cast<Knockback>(spl) : Knockback::Light;
    p.hit.dirY = static_cast<int16_t>(clampedInt(packet, "dirY", -32768, 32767));
    const auto blocked = packet.find("blocked");
    p.hit.blocked = blocked != packet.end() && blocked->is_boolean() && blocked->get<bool>();
    p.hit.viewSeq = static_cast<uint32_t>(clampedInt(packet, "viewSeq", 0, 0xFFFFFFFFll));
    clampToTable(p.hit);
    p.arrivedAt = Clock::now();

    const auto drop = [&](const char* reason) { dropPending(p, reason); };
    if (!session.roomState().pvpMode) {
        return drop("disabled");
    }
    const Client* attacker = findClient(from);
    if (attacker == nullptr) {
        return drop("stage");
    }
    if (!pvpAllowedWith(*attacker)) {
        return drop("team");
    }
    // A hit made before either of us changed stage or layer never lands in the next one.
    const auto stage = packet.find("stageName");
    const bool sameStage = stage != packet.end() && stage->is_string() &&
                           stage->get<std::string>() == session.reportedStageName() &&
                           clampedInt(packet, "layerNo", -128, 127) == session.reportedLayerNo();
    if (!session.clientIsInCurrentLayer(*attacker) || !sameStage) {
        return drop("stage");
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr || !isSaveLoaded() || session.currentSaveTblNo() < 0 ||
        dComIfGp_isEnableNextStage() || localCutsceneRunning())
    {
        return drop("busy");
    }
    // Lag favours the attacker, but only by a pose of ours at most a second old.
    const int32_t age = static_cast<int32_t>(Session::localPoseSeq() - p.hit.viewSeq);
    if (age > kMaxViewAgeTicks || age < -kMaxViewAgeTicks) {
        return drop("stale");
    }
    const float dx = attacker->posX - link->current.pos.x;
    const float dy = attacker->posY - link->current.pos.y;
    const float dz = attacker->posZ - link->current.pos.z;
    const float range = maxRange(p.hit.kind);
    if (dx * dx + dy * dy + dz * dz > range * range) {
        return drop("range");
    }
    if (s_state.pending.size() >= kMaxPending) {
        return drop("busy");
    }
    s_state.pending.push_back(p);
}

void handleDamageResult(const nlohmann::json& packet) {
    const uint32_t self = Session::instance().selfClientId();
    const uint32_t from = static_cast<uint32_t>(clampedInt(packet, "clientId", 0, 0x7FFFFFFF));
    if (clampedInt(packet, "targetClientId", 0, 0x7FFFFFFF) != self || from == 0 || from == self) {
        return;
    }
    const auto it = s_state.attacks.find(from);
    if (it == s_state.attacks.end()) {
        return;  // we never hit them this session
    }
    const uint32_t hitId = static_cast<uint32_t>(clampedInt(packet, "hitId", 0, 0x7FFFFFFF));
    const std::string result = shortString(packet, "result");
    const std::string reason = shortString(packet, "reason");
    const int damage = static_cast<int>(clampedInt(packet, "damage", 0, 80));
    AttackStats& st = it->second;
    if (result == "applied") {
        st.applied++;
        st.damage += damage;
    } else if (result == "blocked") {
        st.blocked++;
    } else if (result == "refused") {
        st.refused++;
    } else {
        st.dropped++;
    }
    if (hitId == st.lastHitId) {
        st.lastAnswered = true;
        st.lastResult = result;
        st.lastReason = reason;
        st.lastDamage = damage;
    }
    TwiliLog.info("[pvp] hit {} on client {}: {}{}{} ({} damage){}", hitId, from, result,
        reason.empty() ? "" : " ", reason, damage,
        packet.contains("fromServer") ? " [server]" : "");
}

}  // namespace

// "" is no team: rooms where nobody set a team fight without friendly fire.
bool pvpAllowedWith(const Client& client) {
    const Session& session = Session::instance();
    const std::string& team = session.selfTeamId();
    return session.roomState().pvpFriendlyFire || team.empty() || team != client.teamId;
}

// One swing can touch all three body cylinders and the head: keep the strongest.
void queueHit(uint32_t victimId, const HitReport& hit) {
    const auto rank = [](const HitReport& h) {
        return std::make_tuple(h.damage, h.knockback, h.blocked);
    };
    const auto [it, inserted] = s_state.outgoing.try_emplace(victimId, hit);
    if (!inserted && rank(hit) > rank(it->second)) {
        it->second = hit;
    }
}

bool takePendingHit(PendingHit& out) {
    const Session& session = Session::instance();
    const auto now = Clock::now();
    while (!s_state.pending.empty()) {
        const PendingHit p = s_state.pending.front();
        s_state.pending.erase(s_state.pending.begin());
        // PvP turned off, the attacker left or changed team since the hit arrived.
        const Client* attacker = findClient(p.attackerId);
        if (!session.roomState().pvpMode) {
            dropPending(p, "disabled");
        } else if (attacker == nullptr || !session.clientIsInCurrentLayer(*attacker)) {
            dropPending(p, "stage");
        } else if (!pvpAllowedWith(*attacker)) {
            dropPending(p, "team");
        } else if (localCutsceneRunning()) {
            dropPending(p, "busy");
        } else if (now - p.arrivedAt > kPendingTtl) {
            dropPending(p, "expired");
        } else {
            out = p;
            return true;
        }
    }
    return false;
}

void requeueHit(const PendingHit& hit) {
    s_state.pending.insert(s_state.pending.begin(), hit);
}

void reportResult(const PendingHit& hit, const char* result, const char* reason, int damage) {
    Session::instance().send({
        {"type", "DAMAGE_RESULT"},
        {"targetClientId", hit.attackerId},
        {"hitId", hit.hitId},
        {"result", result},
        {"reason", reason},
        {"damage", damage},
    });
    VictimStats& st = s_state.victim;
    if (std::strcmp(result, "dropped") == 0) {
        st.dropped++;
        st.lastDropReason = reason;
    } else {
        st.taken++;
        st.damage += damage;
        if (std::strcmp(result, "blocked") == 0) {
            st.blocked++;
        }
    }
    TwiliLog.info("[pvp] hit {} from client {} kind {} dmg {} {}: {}{}{} (took {})", hit.hitId,
        hit.attackerId, kindName(hit.hit.kind), hit.hit.damage, knockbackName(hit.hit.knockback),
        result, reason[0] != '\0' ? " " : "", reason, damage);
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type == "DAMAGE_PLAYER") {
        handleDamagePlayer(packet);
        return true;
    }
    if (type == "DAMAGE_RESULT") {
        handleDamageResult(packet);
        return true;
    }
    return false;
}

void tick() {
    const Session& session = Session::instance();
    const auto now = Clock::now();

    // Victim: hits our damage check never took (i-frames, an event, a menu, loading).
    if (!s_state.pending.empty()) {
        const daAlink_c* link = daAlink_getAlinkActorClass();
        const bool invincible =
            link != nullptr &&
            (link->mDamageTimer != 0 || link->checkModeFlg(daAlink_c::MODE_HIT_STUN));
        for (auto it = s_state.pending.begin(); it != s_state.pending.end();) {
            if (now - it->arrivedAt > kPendingTtl) {
                const PendingHit p = *it;
                it = s_state.pending.erase(it);
                dropPending(p, invincible ? "invincible" : "busy");
            } else {
                ++it;
            }
        }
    }

    // Attacker: what the last collision pass registered.
    std::map<uint32_t, HitReport> outgoing;
    outgoing.swap(s_state.outgoing);
    if (!session.roomState().pvpMode) {
        return;
    }
    for (const auto& [victimId, hit] : outgoing) {
        const Client* victim = findClient(victimId);
        if (victim == nullptr || !victim->hasPlayerUpdate ||
            !session.clientIsInCurrentLayer(*victim) || !pvpAllowedWith(*victim))
        {
            continue;
        }
        auto& lastSent = s_state.lastSentAt[victimId];
        if (lastSent != Clock::time_point{} && now - lastSent < kSendCooldown) {
            continue;
        }
        lastSent = now;
        sendDamagePlayer(victimId, hit);
    }
}

// Hit ids keep counting: a late answer from the old session can never match.
void resetSession() {
    const uint32_t nextHitId = s_state.nextHitId;
    s_state = State{};
    s_state.nextHitId = nextHitId;
}

uint32_t sendDamagePlayerForTest(uint32_t victimId, const HitReport& hit) {
    return sendDamagePlayer(victimId, hit);
}

const AttackStats* attackStats(uint32_t victimId) {
    const auto it = s_state.attacks.find(victimId);
    return it == s_state.attacks.end() ? nullptr : &it->second;
}

const VictimStats& victimStats() {
    return s_state.victim;
}

}  // namespace twili::pvp
