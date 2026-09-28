// Scales the health pool (not incoming damage), so wolf bites and clawshot pulls scale too. Live
// enemies are rescaled whenever the percent changes, keeping their health fraction; health an
// enemy gives itself again (getting up, reviving) is scaled by the watch in tick().

#include "enemy/EnemyScaling.hpp"

#include "core/Session.hpp"

#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <unordered_map>

namespace twili::enemy_scaling {
namespace {

// Headroom below the s16 limit for bonuses added right before cc_at_check (e_oc +140, e_ww +180).
constexpr int kMaxScaledHealth = 30000;

struct Tracked {
    fopAc_ac_c* actor;  // valid until onActorDeleted
    s16 baseMax;
    // field_0x560 as we last wrote it; 0 = not ours to manage.
    s16 lastMax;
    s16 lastHealth;
    int appliedPct;
};

std::unordered_map<fpc_ProcID, Tracked> s_tracked;

s16 clampHealth(int v) {
    return static_cast<s16>(std::clamp(v, 1, kMaxScaledHealth));
}

s16 scaledValue(int v, int pct) {
    return clampHealth((v * pct + 50) / 100);
}

// Enemies whose health really decides when they die; each entry was checked for the create-time
// assignment, every later write to health and the damage paths.
bool isHealthScalable(fopAc_ac_c* ac) {
    const u8 arg0 = fopAcM_GetParam(ac) & 0xFF;
    switch (fopAcM_GetName(ac)) {
    case fpcNm_E_OC_e:   // Bokoblin
    case fpcNm_E_RDY_e:  // Shadow Bulblin
    case fpcNm_E_MF_e:   // Dynalfos
    case fpcNm_E_DN_e:   // Lizalfos
    case fpcNm_E_S1_e:   // Shadow Beast
    case fpcNm_E_BS_e:   // Stalkin
    case fpcNm_E_DD_e:   // Dodongo
    case fpcNm_E_GB_e:   // Giant Baba
    case fpcNm_E_KK_e:   // Chilfos
    case fpcNm_E_MM_e:   // Helmasaur (its shell breaks after field_0x560 damage)
    case fpcNm_E_SF_e:   // Stalfos
    case fpcNm_E_ST_e:   // Skulltula (the small variant has no health)
    case fpcNm_E_GI_e:   // Gibdo
    case fpcNm_E_KR_e:   // Kargorok
    case fpcNm_E_SH_e:   // Stalhound
    case fpcNm_E_YC_e:   // Twilight Kargorok
    case fpcNm_E_HZ_e:   // Tile Worm
    case fpcNm_E_KG_e:   // Young Gohma
    case fpcNm_E_BU_e:   // Bubble
    case fpcNm_E_WW_e:   // White Wolfos (the 1-health variant is skipped)
    case fpcNm_E_FS_e:   // Wooden Puppet
    case fpcNm_E_FZ_e:   // Mini Freezard
    case fpcNm_E_SM_e:   // Chu Worm
        return true;
    case fpcNm_E_RD_e:
        // Bulblin; King Bulblin (4, 5, 11, 12) is reset to 100 on every damage check.
        return arg0 != 4 && arg0 != 5 && arg0 != 11 && arg0 != 12;
    case fpcNm_E_PO_e:
        // Poe; 3-14 are scripted variants.
        return arg0 < 3 || arg0 > 14;
    default:
        return false;
    }
}

// Health of 1 or less is a dying state many enemies test for: left alone, and a lower percent
// never rounds a live enemy into it.
void rebake(Tracked& t, int pct) {
    fopAc_ac_c* ac = t.actor;
    if (ac->health > 1) {
        ac->health = static_cast<s16>(
            std::clamp((ac->health * pct + t.appliedPct / 2) / t.appliedPct, 2, kMaxScaledHealth));
    }
    if (t.lastMax != 0 && ac->field_0x560 == t.lastMax) {
        ac->field_0x560 = scaledValue(t.baseMax, pct);
        t.lastMax = ac->field_0x560;
    } else {
        // The enemy wrote its max itself since (or never had one).
        t.lastMax = 0;
    }
    t.appliedPct = pct;
    t.lastHealth = ac->health;
}

}  // namespace

int healthPercent() {
    if (!Session::active()) {
        return 100;
    }
    const Session& session = Session::instance();
    // The room state arrives with the roster that assigns our client id.
    if (!session.joined()) {
        return 100;
    }
    return std::clamp(session.roomState().enemyHealthMultiplier, 100, 500);
}

void onActorCreated(fopAc_ac_c* ac) {
    if (!isHealthScalable(ac)) {
        return;
    }
    const s16 max = ac->field_0x560;
    const s16 base = max > 0 ? max : ac->health;
    // One-hit variants and variants without health.
    if (base <= 1 || ac->health <= 1) {
        return;
    }
    Tracked t{ac, base, max > 0 ? max : s16(0), ac->health, 100};
    // Tracked offline too, so connecting later rescales it.
    rebake(t, healthPercent());
    s_tracked[fopAcM_GetID(ac)] = t;
}

void onActorDeleted(fopAc_ac_c* ac) {
    s_tracked.erase(fopAcM_GetID(ac));
}

void onHealthReset(fopAc_ac_c* ac) {
    const auto it = s_tracked.find(fopAcM_GetID(ac));
    if (it == s_tracked.end()) {
        return;
    }
    if (ac->health > 1) {
        ac->health = scaledValue(ac->health, it->second.appliedPct);
    }
    // So the watch does not scale it a second time.
    it->second.lastHealth = ac->health;
}

void tick() {
    const int pct = healthPercent();
    for (auto& [id, t] : s_tracked) {
        fopAc_ac_c* ac = t.actor;
        // Back to its unscaled full value is scaled full; any other gain is scaled as a gain.
        if (t.appliedPct != 100 && ac->health > t.lastHealth) {
            const int from = std::max<int>(t.lastHealth, 0);
            ac->health = ac->health == t.baseMax ?
                             scaledValue(t.baseMax, t.appliedPct) :
                             clampHealth(from + ((ac->health - from) * t.appliedPct + 50) / 100);
        }
        t.lastHealth = ac->health;
        if (pct != t.appliedPct) {
            rebake(t, pct);
        }
    }
}

// Unloading: live enemies go back to vanilla health.
void shutdown() {
    for (auto& [id, t] : s_tracked) {
        if (t.appliedPct != 100) {
            rebake(t, 100);
        }
    }
    s_tracked.clear();
}

bool trackedHealth(fpc_ProcID id, TrackedHealth& out) {
    const auto it = s_tracked.find(id);
    if (it == s_tracked.end()) {
        return false;
    }
    out = {it->second.baseMax, it->second.appliedPct};
    return true;
}

}  // namespace twili::enemy_scaling
