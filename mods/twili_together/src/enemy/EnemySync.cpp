// Shares the death of an allowlisted enemy; the receiver deletes its copy with a drop-less puff.

#include "enemy/EnemySync.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "enemy/EnemyCount.hpp"

#include "m_Do/m_Do_ext.h"  // the enemy headers are not self-contained

#include "d/actor/d_a_e_hz.h"
#include "d/actor/d_a_e_s1.h"
#include "d/actor/d_a_player.h"
#include "d/d_bg_w.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_event.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace twili::enemy_sync {
namespace {

using Clock = std::chrono::steady_clock;

// e_rdy deletes 50 frames after its puff, the e_s1 field demo 57.
constexpr auto kPairWindow = std::chrono::seconds(6);
// Our copy's room may still be loading.
constexpr auto kPendingTtl = std::chrono::seconds(10);
// Waiting out a local event or a grab.
constexpr auto kMaxDefer = std::chrono::seconds(60);
// Echo and double-kill suppression.
constexpr auto kRecentTtl = std::chrono::seconds(8);
// A puff this recent means "dying".
constexpr auto kStillDying = std::chrono::seconds(2);
constexpr float kHomeTolerance = 1.0f;
constexpr size_t kMaxPending = 64;
constexpr size_t kMaxRecent = 256;

// Enemies that die without being deleted: synced as "defeated in place".
struct InPlaceDeath {
    s16 procName;
    bool (*isDead)(const fopAc_ac_c*);
    void (*kill)(fopAc_ac_c*);
};

bool hzIsDead(const fopAc_ac_c* ac) {
    return static_cast<const daE_HZ_c*>(ac)->field_0x6e8 != 0;
}

// executeDeathWait's created-dead branch: no switch write (the killer's comes by world sync).
void hzKill(fopAc_ac_c* ac) {
    auto* hz = static_cast<daE_HZ_c*>(ac);
    // Its execute stops running, so nothing would release a force lock later.
    if (dCamera_c* cam = dCam_getBody(); cam != nullptr && cam->GetForceLockOnActor() == ac) {
        cam->ForceLockOff(ac);
    }
    if (hz->mpBgW != nullptr) {
        dComIfG_Bgsp().Release(hz->mpBgW);
        hz->mpBgW = nullptr;
    }
    hz->setActionMode(11);  // ACTION_DEATH_WAIT
    hz->mMode = 1;
}

constexpr InPlaceDeath kInPlace[] = {
    {fpcNm_E_HZ_e, hzIsDead, hzKill},  // Tile Worm: keeps drawing its tile plate
};

const InPlaceDeath* findInPlace(s16 procName) {
    for (const InPlaceDeath& d : kInPlace) {
        if (d.procName == procName) {
            return &d;
        }
    }
    return nullptr;
}

struct Record {
    fopAc_ac_c* actor;  // valid until onActorDeleted
    SpawnKey key;
    const InPlaceDeath* inPlace = nullptr;
    char stage[8];
    int8_t layer;
    uint8_t fxSize = 10, fxType = 0;
    Clock::time_point disappearAt{}, deleteAt{};
    // Explicit mark: dies without a puff.
    bool defeated = false;
    // Sent, or dying by its own hand when a teammate's kill came.
    bool reported = false;
    bool remoteRemoved = false;
};

struct Out {
    Kill kill;
    char stage[8];
    int8_t layer;
};

struct Pending {
    Kill kill;
    Clock::time_point at;
    bool deferred = false;
};

std::unordered_map<fpc_ProcID, Record> s_records;
std::vector<Out> s_outbox;
std::vector<Pending> s_pending;
std::vector<std::pair<SpawnKey, Clock::time_point>> s_recent;
// Around our own fopAcM_delete of a remote kill, so the hook does not stamp it.
bool s_applyingRemote = false;
Stats s_stats{};

void logKill(const char* what, const Kill& k) {
    TwiliLog.info("[enemy] {} ({})", what, keyText(k.key));
}

// An explicit allowlist: the ENEMY group also holds bosses and mini-bosses.
bool isKillSyncable(s16 procName, u32 params) {
    switch (procName) {
    case fpcNm_E_OC_e:   // Bokoblin (its fall death has no puff: markDefeated)
    case fpcNm_E_MF_e:   // Dynalfos
    case fpcNm_E_DN_e:   // Lizalfos
    case fpcNm_E_KR_e:   // Kargorok
    case fpcNm_E_SF_e:   // Stalfos (only the final death puffs)
    case fpcNm_E_ST_e:   // Skulltula
    case fpcNm_E_MM_e:   // Helmasaur
    case fpcNm_E_DD_e:   // Dodongo
    case fpcNm_E_KK_e:   // Chilfos
    case fpcNm_E_GI_e:   // Gibdo
    case fpcNm_E_SH_e:   // Stalhound
    case fpcNm_E_WW_e:   // White Wolfos
    case fpcNm_E_FS_e:   // Wooden Puppet
    case fpcNm_E_SM_e:   // Chu Worm
    case fpcNm_E_S1_e:   // Shadow Beast (the group goes through onShadowBeastGroupDown)
    case fpcNm_E_BA_e:   // Keese
    case fpcNm_E_DB_e:   // Deku Baba (sets its zone actor bit)
    case fpcNm_E_TT_e:   // Tektite
    case fpcNm_E_BU_e:   // Bubble
    case fpcNm_E_KG_e:   // Young Gohma
    case fpcNm_E_BS_e:   // Stalkin (no puff: markDefeated)
    case fpcNm_E_HZ_e:   // Tile Worm (defeated in place)
        return true;
    case fpcNm_E_RD_e: {
        // Bulblin, not King Bulblin (4, 5, 11, 12): scripted fights.
        const u8 arg0 = params & 0xFF;
        return arg0 != 4 && arg0 != 5 && arg0 != 11 && arg0 != 12;
    }
    case fpcNm_E_RDY_e:
        // Shadow Bulblin, not variant 12: the story Kargorok keeps a raw pointer to its rider.
        return ((params & 0xF000) >> 12) != 12;
    default:
        return false;
    }
}

// isActor/onActor assert a setID below ACTOR_MAX and a live zone for the room.
bool zoneOk(const SpawnKey& k) {
    if (k.setId >= dSv_zoneActor_c::ACTOR_MAX || k.roomNo < 0 || k.roomNo >= 64) {
        return false;
    }
    const int zoneNo = dComIfGp_roomControl_getZoneNo(k.roomNo);
    return zoneNo >= 0 && zoneNo < dSv_info_c::ZONE_MAX;
}

bool isUnset(Clock::time_point t) {
    return t == Clock::time_point{};
}

// Bounded: every remote kill scans it, and nothing limits how many a teammate sends.
void noteRecent(const SpawnKey& key) {
    if (s_recent.size() >= kMaxRecent) {
        s_recent.erase(s_recent.begin());
    }
    s_recent.push_back({key, Clock::now()});
}

void report(Record& r, bool zoneActor) {
    r.reported = true;
    noteRecent(r.key);
    Out o{{r.key, r.fxSize, r.fxType, zoneActor}, {}, r.layer};
    std::memcpy(o.stage, r.stage, sizeof(o.stage));
    s_outbox.push_back(o);
    TwiliLog.info("[enemy] {} defeated ({}{})", fopAcM_getProcNameString(r.actor), keyText(r.key),
        zoneActor ? ", zone actor bit" : "");
}

Record* live(fpc_ProcID id) {
    if (s_applyingRemote) {
        return nullptr;
    }
    const auto it = s_records.find(id);
    return it == s_records.end() ? nullptr : &it->second;
}

Record* live(const fopAc_ac_c* ac) {
    return ac == nullptr ? nullptr : live(fopAcM_GetID(ac));
}

enum class Apply { Done, NoMatch, Deferred };

// createDisappear's parameters, enemy id 0xFF (no item roll), queued in the enemy's layer.
void spawnPuff(fopAc_ac_c* ac, layer_class* layer, const Kill& kill) {
    if (layer == nullptr) {
        return;
    }
    const u32 prm = (0xFFu << 16) | (u32(kill.fxSize) << 8) | kill.fxType;
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(layer);
    fopAcM_create(fpcNm_DISAPPEAR_e, prm, &ac->current.pos, fopAcM_GetRoomNo(ac),
        &ac->current.angle, nullptr, -1);
    fpcLy_SetCurrentLayer(saved);
}

// Why removing the enemy must wait (our cutscene, Link holds or bites it), or nullptr.
const char* deferReason(fopAc_ac_c* ac) {
    if (dComIfGp_event_runCheck()) {
        switch (dComIfGp_event_getMode()) {
        case dEvt_mode_TALK_e:
            return "a conversation is running";
        case dEvt_mode_DEMO_e:
            return "a cutscene is running";
        default:
            return "an event is running";
        }
    }
    daPy_py_c* player = daPy_getPlayerActorClass();
    if (player == nullptr) {
        return "no player";
    }
    if (player->getGrabActorID() == fopAcM_GetID(ac)) {
        return "Link is holding it";
    }
    if (player->checkWolfEnemyBiteAllOwn(ac)) {
        return "Link is biting it";
    }
    return nullptr;
}

Apply tryApply(const Pending& p, const char*& why) {
    const auto now = Clock::now();
    for (auto& [id, r] : s_records) {
        if (r.reported || r.remoteRemoved || !sameKey(r.key, p.kill.key)) {
            continue;
        }
        // Before the IsExecuting skip: an unconsumed kill would remove a respawned enemy.
        const bool executing = fopAcM_IsExecuting(id);
        if (r.defeated || (!isUnset(r.deleteAt) && !executing) ||
            (!isUnset(r.disappearAt) && now - r.disappearAt < kStillDying))
        {
            // Dying by its own hand here: let it finish with its own drop, and do not report it.
            r.reported = true;
            return Apply::Done;
        }
        if (!executing) {
            continue;
        }
        fopAc_ac_c* ac = r.actor;
        if (r.inPlace != nullptr && r.inPlace->isDead(ac)) {
            return Apply::Done;
        }
        why = deferReason(ac);
        if (why != nullptr) {
            return Apply::Deferred;
        }
        if (r.inPlace != nullptr) {
            r.inPlace->kill(ac);
            r.remoteRemoved = true;
            spawnPuff(ac, ac->layer_tag.layer, p.kill);
            ++s_stats.applied;
            TwiliLog.info("[enemy] {} ({}) defeated in place by a teammate",
                fopAcM_getProcNameString(ac), keyText(r.key));
            return Apply::Done;
        }
        // The actor stays readable until fopAc_Delete runs next frame.
        layer_class* layer = ac->layer_tag.layer;
        s_applyingRemote = true;
        const bool deleted = fopAcM_delete(ac) != 0;
        s_applyingRemote = false;
        if (!deleted) {
            why = "its delete was refused";
            return Apply::Deferred;
        }
        r.remoteRemoved = true;
        spawnPuff(ac, layer, p.kill);
        ++s_stats.applied;
        TwiliLog.info("[enemy] removed {} ({}): defeated by a teammate",
            fopAcM_getProcNameString(ac), keyText(r.key));
        return Apply::Done;
    }
    return Apply::NoMatch;
}

}  // namespace

std::string keyText(const SpawnKey& k) {
    return fmt::format("proc {}, room {}, setId 0x{:04X}, home {:.1f} {:.1f} {:.1f}{}", k.procName,
        k.roomNo, k.setId, k.home[0], k.home[1], k.home[2],
        k.dup != 0 ? fmt::format(", extra {}", k.dup) : std::string{});
}

bool sameKey(const SpawnKey& a, const SpawnKey& b) {
    return a.procName == b.procName && a.roomNo == b.roomNo && a.params == b.params &&
           a.setId == b.setId && a.dup == b.dup &&
           std::fabs(a.home[0] - b.home[0]) <= kHomeTolerance &&
           std::fabs(a.home[1] - b.home[1]) <= kHomeTolerance &&
           std::fabs(a.home[2] - b.home[2]) <= kHomeTolerance;
}

bool appendKey(const fopAc_ac_c* actor, SpawnKey& out) {
    auto* ac = const_cast<fopAc_ac_c*>(actor);
    const fopAcM_prm_class* prm = fopAcM_GetAppend(ac);
    // Children of other actors spawn where the parent's timing puts them.
    if (prm == nullptr || prm->parent_id != fpcM_ERROR_PROCESS_ID_e) {
        return false;
    }
    const cXyz home = prm->base.position;
    out = {fopAcM_GetName(ac), prm->room_no, static_cast<uint32_t>(prm->base.parameters),
        static_cast<uint16_t>(prm->base.setID), {home.x, home.y, home.z}};
    return true;
}

const SpawnKey* trackedKey(fpc_ProcID id) {
    const auto it = s_records.find(id);
    return it == s_records.end() ? nullptr : &it->second.key;
}

bool isDying(fpc_ProcID id) {
    const auto it = s_records.find(id);
    if (it == s_records.end()) {
        return false;
    }
    const Record& r = it->second;
    return r.defeated || r.reported || r.remoteRemoved || !isUnset(r.deleteAt) ||
           (!isUnset(r.disappearAt) && Clock::now() - r.disappearAt < kStillDying) ||
           (r.inPlace != nullptr && r.inPlace->isDead(r.actor));
}

void onActorCreated(fopAc_ac_c* ac) {
    if (!isKillSyncable(fopAcM_GetName(ac), fopAcM_GetParam(ac))) {
        return;
    }
    const char* stage = dComIfGp_getStartStageName();
    Record r{};
    // An extra goes by its original's key; others by the spawn data, readable only now.
    if (stage == nullptr ||
        (!enemy_count::extraKey(fopAcM_GetID(ac), r.key) && !appendKey(ac, r.key)))
    {
        return;
    }
    r.actor = ac;
    r.inPlace = findInPlace(r.key.procName);
    std::strncpy(r.stage, stage, sizeof(r.stage) - 1);
    r.layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    s_records[fopAcM_GetID(ac)] = r;
}

void onDisappear(const fopAc_ac_c* ac, uint8_t size, uint8_t type) {
    if (Record* r = live(ac)) {
        r->disappearAt = Clock::now();
        r->fxSize = size;
        r->fxType = type;
    }
}

void onDeleteRequest(const fopAc_ac_c* ac) {
    if (Record* r = live(ac); r != nullptr && isUnset(r->deleteAt)) {
        r->deleteAt = Clock::now();
    }
}

void onDeleteRequest(fpc_ProcID id) {
    if (Record* r = live(id); r != nullptr && isUnset(r->deleteAt)) {
        r->deleteAt = Clock::now();
    }
}

void markDefeated(const fopAc_ac_c* ac) {
    if (Record* r = live(ac)) {
        r->defeated = true;
    }
}

void onActorDeleted(fopAc_ac_c* ac) {
    const auto it = s_records.find(fopAcM_GetID(ac));
    if (it == s_records.end()) {
        return;
    }
    Record r = it->second;
    s_records.erase(it);
    if (r.reported || r.remoteRemoved) {
        return;
    }
    const bool paired = !isUnset(r.disappearAt) && !isUnset(r.deleteAt) &&
                        (r.disappearAt > r.deleteAt ? r.disappearAt - r.deleteAt :
                                                      r.deleteAt - r.disappearAt) <= kPairWindow;
    if (!r.defeated && !paired) {
        return;  // room unload, scene teardown, despawn
    }
    // A frame after the delete request: e_db sets its zone actor bit right after asking.
    report(r, zoneOk(r.key) && dComIfGs_isActor(r.key.setId, r.key.roomNo));
}

void onShadowBeastGroupDown(const e_s1_class* self) {
    // Each beast dies 30-40 frames from now; teammates lose the whole group together.
    for (auto& [id, r] : s_records) {
        if (r.key.procName != fpcNm_E_S1_e || r.reported || r.remoteRemoved) {
            continue;
        }
        if (static_cast<const e_s1_class*>(r.actor)->mGroupID != self->mGroupID) {
            continue;
        }
        r.defeated = true;
        // d_a_e_s1.cpp e_s1_fail
        r.fxSize = 12;
        r.fxType = 1;
        report(r, false);
    }
}

void flush() {
    if (s_outbox.empty()) {
        return;
    }
    const Session& session = Session::instance();
    if (session.joined() && session.roomState().syncNPCs && isSaveLoaded()) {
        // One packet per run of kills from the same stage and layer.
        Kill batch[kMaxKillsPerPacket];
        size_t i = 0;
        while (i < s_outbox.size()) {
            const Out& first = s_outbox[i];
            size_t n = 0;
            while (i < s_outbox.size() && n < kMaxKillsPerPacket &&
                   std::strncmp(s_outbox[i].stage, first.stage, sizeof(first.stage)) == 0 &&
                   s_outbox[i].layer == first.layer)
            {
                batch[n++] = s_outbox[i++].kill;
            }
            if (detail::sendDefeated(first.stage, first.layer, batch, n)) {
                s_stats.sent += static_cast<uint32_t>(n);
            }
        }
    }
    // Kills made offline or with the option off are never sent later.
    s_outbox.clear();
}

void detail::queueRemote(const Kill& kill) {
    ++s_stats.received;
    if (!isKillSyncable(kill.key.procName, kill.key.params)) {
        return;
    }
    for (const auto& [key, at] : s_recent) {
        if (sameKey(key, kill.key)) {
            ++s_stats.echoes;
            return;
        }
    }
    noteRecent(kill.key);
    logKill("a teammate defeated an enemy", kill);
    // Mirrored without a live copy too: the bit keeps it from coming back with its room.
    if (kill.zoneActor && zoneOk(kill.key)) {
        dComIfGs_onActor(kill.key.setId, kill.key.roomNo);
    }
    if (s_pending.size() >= kMaxPending) {
        s_pending.erase(s_pending.begin());
    }
    s_pending.push_back({kill, Clock::now()});
}

void tick() {
    const auto now = Clock::now();
    std::erase_if(s_recent, [&](const auto& e) { return now - e.second > kRecentTtl; });
    // Created dead never puffs, so never reports.
    for (auto& [id, r] : s_records) {
        if (r.inPlace != nullptr && !r.reported && !r.remoteRemoved &&
            !isUnset(r.disappearAt) && now - r.disappearAt <= kPairWindow &&
            fopAcM_IsExecuting(id) && r.inPlace->isDead(r.actor))
        {
            report(r, zoneOk(r.key) && dComIfGs_isActor(r.key.setId, r.key.roomNo));
        }
    }
    const Session& session = Session::instance();
    if (!session.isConnected() || !session.roomState().syncNPCs || !isSaveLoaded()) {
        s_pending.clear();
        return;
    }
    for (auto it = s_pending.begin(); it != s_pending.end();) {
        if (now - it->at > (it->deferred ? kMaxDefer : kPendingTtl)) {
            ++s_stats.expired;
            logKill(it->deferred ? "a teammate's kill expired, never allowed to apply" :
                                   "a teammate's kill expired, no matching enemy here",
                it->kill);
            it = s_pending.erase(it);
            continue;
        }
        const char* why = nullptr;
        switch (tryApply(*it, why)) {
        case Apply::Done:
            it = s_pending.erase(it);
            break;
        case Apply::Deferred:
            if (!it->deferred) {
                TwiliLog.info("[enemy] removing a teammate's kill waits: {}", why);
            }
            it->deferred = true;
            ++it;
            break;
        case Apply::NoMatch:
            ++it;
            break;
        }
    }
}

void resetSession() {
    // Records stay: they mirror live actors, connected or not.
    s_outbox.clear();
    s_pending.clear();
    s_recent.clear();
}

void shutdown() {
    resetSession();
    s_records.clear();
}

const Stats& stats() {
    return s_stats;
}

}  // namespace twili::enemy_sync
