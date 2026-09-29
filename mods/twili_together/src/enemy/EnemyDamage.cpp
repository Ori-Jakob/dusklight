// Measures each shared enemy's health loss per update; a teammate's loss is a plain health write here.

#include "enemy/EnemyDamage.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "enemy/EnemyScaling.hpp"

#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace twili::enemy_damage {
namespace {

using Clock = std::chrono::steady_clock;

// A health drop this soon after our hit or bite is our damage.
constexpr auto kAttribution = std::chrono::milliseconds(500);
// Our copy's room may still be loading.
constexpr auto kPendingTtl = std::chrono::seconds(2);
constexpr size_t kMaxPending = 64;
constexpr int kMaxScaledHealth = 30000;

struct Rec {
    fopAc_ac_c* actor;  // valid until onActorDeleted
    enemy_sync::SpawnKey key;
    char stage[8];
    int8_t layer;
    int16_t floor;
    int16_t lastHealth;
    int lastPct;
    Clock::time_point hitAt{}, biteAt{};
};

struct Out {
    Hit hit;
    char stage[8];
    int8_t layer;
};

struct Pending {
    Hit hit;
    Clock::time_point at;
};

std::unordered_map<fpc_ProcID, Rec> s_recs;
std::vector<Out> s_outbox;
std::vector<Pending> s_pending;
Stats s_stats{};

// Kill-synced and health-scaled, minus enemies whose health is also a state (revive, shell, counter).
bool isDamageShareable(s16 procName, u32 params) {
    switch (procName) {
    case fpcNm_E_OC_e:   // Bokoblin
    case fpcNm_E_MF_e:   // Dynalfos
    case fpcNm_E_DN_e:   // Lizalfos
    case fpcNm_E_KR_e:   // Kargorok
    case fpcNm_E_DD_e:   // Dodongo
    case fpcNm_E_KK_e:   // Chilfos
    case fpcNm_E_GI_e:   // Gibdo
    case fpcNm_E_SH_e:   // Stalhound
    case fpcNm_E_WW_e:   // White Wolfos
    case fpcNm_E_BU_e:   // Bubble
    case fpcNm_E_KG_e:   // Young Gohma
    case fpcNm_E_BS_e:   // Stalkin
    case fpcNm_E_ST_e:   // Skulltula
    case fpcNm_E_FS_e:   // Wooden Puppet
    case fpcNm_E_HZ_e:   // Tile Worm
        return true;
    case fpcNm_E_RD_e: {
        // Not King Bulblin.
        const u8 arg0 = params & 0xFF;
        return arg0 != 4 && arg0 != 5 && arg0 != 11 && arg0 != 12;
    }
    case fpcNm_E_RDY_e:
        // Not the Twilit Kargorok's rider.
        return ((params & 0xF000) >> 12) != 12;
    default:
        return false;
    }
}

// Puppet and Skulltula zero themselves at 10 or less; Chilfos ignores hits at 1.
int16_t floorOf(s16 procName) {
    return procName == fpcNm_E_FS_e || procName == fpcNm_E_ST_e ? 11 : 2;
}

int appliedPct(fpc_ProcID id) {
    enemy_scaling::TrackedHealth t{};
    return enemy_scaling::trackedHealth(id, t) ? t.appliedPct : 100;
}

// A rescale is no damage: the baseline follows it, rounded as enemy_scaling rounds.
void followPct(Rec& r, int pct) {
    if (pct == r.lastPct) {
        return;
    }
    if (r.lastHealth > 1) {
        r.lastHealth = static_cast<int16_t>(std::clamp(
            (r.lastHealth * pct + r.lastPct / 2) / r.lastPct, 2, kMaxScaledHealth));
    }
    r.lastPct = pct;
}

bool optionOn() {
    const RoomState& room = Session::instance().roomState();
    return room.syncNPCs && room.syncEnemyDamage && isSaveLoaded();
}

bool recent(Clock::time_point t, Clock::time_point now) {
    return t != Clock::time_point{} && now - t < kAttribution;
}

void flush() {
    if (s_outbox.empty()) {
        return;
    }
    if (Session::instance().joined() && optionOn()) {
        Hit batch[kMaxHitsPerPacket];
        size_t i = 0;
        while (i < s_outbox.size()) {
            const Out& first = s_outbox[i];
            size_t n = 0;
            while (i < s_outbox.size() && n < kMaxHitsPerPacket &&
                   std::strncmp(s_outbox[i].stage, first.stage, sizeof(first.stage)) == 0 &&
                   s_outbox[i].layer == first.layer)
            {
                batch[n++] = s_outbox[i++].hit;
            }
            if (detail::sendHits(first.stage, first.layer, batch, n)) {
                s_stats.sent += static_cast<uint32_t>(n);
            }
        }
    }
    s_outbox.clear();
}

enum class Apply { Done, NoMatch };

Apply tryApply(const Hit& h) {
    for (auto& [id, r] : s_recs) {
        if (!enemy_sync::sameKey(r.key, h.key) || !fopAcM_IsExecuting(id)) {
            continue;
        }
        fopAc_ac_c* ac = r.actor;
        const int pct = appliedPct(id);
        followPct(r, pct);
        const int hp = ac->health;
        // Raising a knocked-down or dying copy would revive it.
        if (enemy_sync::isDying(id) || hp <= r.floor) {
            ++s_stats.dropped;
            return Apply::Done;
        }
        const int dmg = std::max(1, (h.dmg * pct + h.pct / 2) / h.pct);
        int next = hp - dmg;
        // Never above the sender's own health (past its rounding): heals a hit we missed.
        bool healed = false;
        if (const int senderHp = (h.hpAfter * pct + 50) / 100;
            h.hpAfter >= 0 && senderHp + pct / 100 < next)
        {
            next = senderHp;
            healed = true;
        }
        const bool floored = next < r.floor;
        if (floored) {
            next = r.floor;
        }
        ac->health = static_cast<s16>(next);
        // Keeps our own damage since the last poll measurable, and never sends this back.
        r.lastHealth = static_cast<int16_t>(r.lastHealth - (hp - next));
        ++s_stats.applied;
        if (floored) {
            ++s_stats.floored;
        }
        if (healed) {
            ++s_stats.healed;
        }
        // A small spark where the teammate's hit landed on their copy.
        const csXyz rot(0, 0, 0);
        dComIfGp_setHitMark(7, nullptr, &ac->eyePos, &rot, nullptr, 0);
        TwiliLog.debug("[enemy] {} ({}) took {} from a teammate: {} -> {}{}",
            fopAcM_getProcNameString(ac), enemy_sync::keyText(r.key), dmg, hp, next,
            floored ? " (floor)" : "");
        return Apply::Done;
    }
    return Apply::NoMatch;
}

}  // namespace

void onActorCreated(fopAc_ac_c* ac) {
    const fpc_ProcID id = fopAcM_GetID(ac);
    const enemy_sync::SpawnKey* key = enemy_sync::trackedKey(id);
    const char* stage = dComIfGp_getStartStageName();
    if (key == nullptr || stage == nullptr || !isDamageShareable(key->procName, key->params)) {
        return;
    }
    const int16_t floor = floorOf(key->procName);
    // Variants without health or dying in one hit.
    if (ac->health <= floor) {
        return;
    }
    Rec r{};
    r.actor = ac;
    r.key = *key;
    std::strncpy(r.stage, stage, sizeof(r.stage) - 1);
    r.layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    r.floor = floor;
    r.lastHealth = ac->health;
    r.lastPct = appliedPct(id);
    s_recs[id] = r;
}

void onActorDeleted(fopAc_ac_c* ac) {
    s_recs.erase(fopAcM_GetID(ac));
}

void onLocalHit(fopAc_ac_c* ac) {
    if (const auto it = s_recs.find(fopAcM_GetID(ac)); it != s_recs.end()) {
        it->second.hitAt = Clock::now();
    }
}

void poll() {
    const auto now = Clock::now();
    const bool share = Session::instance().joined() && optionOn();
    daPy_py_c* player = daPy_getPlayerActorClass();
    for (auto& [id, r] : s_recs) {
        if (!fopAcM_IsExecuting(id)) {
            continue;
        }
        const int pct = appliedPct(id);
        followPct(r, pct);
        // Hang-bite damage is applied inside the enemy's own execute.
        if (player != nullptr && player->checkWolfEnemyBiteAllOwn(r.actor)) {
            r.biteAt = now;
        }
        const int cur = r.actor->health;
        // A lethal loss is a kill: enemy_sync reports it.
        if (share && cur < r.lastHealth && cur > 0 &&
            (recent(r.hitAt, now) || recent(r.biteAt, now)) && !enemy_sync::isDying(id))
        {
            Out o{};
            o.hit = {r.key, static_cast<uint16_t>(r.lastHealth - cur), static_cast<uint16_t>(pct),
                static_cast<int16_t>((cur * 100 + pct / 2) / pct)};
            std::memcpy(o.stage, r.stage, sizeof(o.stage));
            o.layer = r.layer;
            s_outbox.push_back(o);
        }
        r.lastHealth = static_cast<int16_t>(cur);
    }
    flush();
}

void detail::queueRemote(const Hit& hit) {
    ++s_stats.received;
    // Invincible Enemies zeroes our own hits too.
    if (!isDamageShareable(hit.key.procName, hit.key.params) ||
        config::hostBool("game.invincibleEnemies", false))
    {
        ++s_stats.dropped;
        return;
    }
    if (s_pending.size() >= kMaxPending) {
        s_pending.erase(s_pending.begin());
        ++s_stats.dropped;
    }
    s_pending.push_back({hit, Clock::now()});
}

void tick() {
    if (s_pending.empty()) {
        return;
    }
    if (!Session::instance().isConnected() || !optionOn()) {
        s_pending.clear();
        return;
    }
    const auto now = Clock::now();
    for (auto it = s_pending.begin(); it != s_pending.end();) {
        if (tryApply(it->hit) == Apply::Done) {
            it = s_pending.erase(it);
        } else if (now - it->at > kPendingTtl) {
            ++s_stats.dropped;
            TwiliLog.debug("[enemy] a teammate's hit expired, no matching enemy here ({})",
                enemy_sync::keyText(it->hit.key));
            it = s_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void resetSession() {
    // Records stay: they mirror live actors, connected or not.
    s_outbox.clear();
    s_pending.clear();
}

void shutdown() {
    resetSession();
    s_recs.clear();
}

const Stats& stats() {
    return s_stats;
}

void noteLocalHitForTest(fopAc_ac_c* ac) {
    onLocalHit(ac);
}

bool sendForTest(fopAc_ac_c* ac, int dmg, int pct, int hpAfter) {
    const auto it = s_recs.find(fopAcM_GetID(ac));
    if (it == s_recs.end()) {
        return false;
    }
    const Rec& r = it->second;
    const Hit hit{r.key, static_cast<uint16_t>(dmg), static_cast<uint16_t>(pct),
        static_cast<int16_t>(hpAfter)};
    return detail::sendHits(r.stage, r.layer, &hit, 1);
}

}  // namespace twili::enemy_damage
