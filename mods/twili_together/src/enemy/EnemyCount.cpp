// Extras are made once their original ran, from its spawn data and a hash of its key (never the game
// RNG), so every client gets the same ones.

#include "enemy/EnemyCount.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"

#include "m_Do/m_Do_ext.h"

#include "JSystem/JKernel/JKRExpHeap.h"
#include "SSystem/SComponent/c_malloc.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace twili::enemy_count {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxExtrasPerEnemy = 4;
constexpr int kMaxExtrasPerRoom = 24;
constexpr u32 kMinFreeHeap = 4u * 1024 * 1024;
constexpr size_t kMaxPlaced = 512;
// The original runs a few updates first: its room's collision is up by then.
constexpr int kSettleUpdates = 3;
// An extra that fell or strayed this far can hold a room clear forever.
constexpr float kMaxFall = 1000.0f;
constexpr float kMaxStray = 3000.0f;
constexpr auto kStrayTime = std::chrono::seconds(10);

struct DupSpec {
    s16 procName;
    // Where the "defeated" switch lives: forced to 0xFF in extras.
    u32 switchMask;
    bool switchInAngleZ;
    bool needsGround;
    bool (*accepts)(u32 params);
};

// Type 2 is the bridge lookout, whose switch is elsewhere.
bool bokoblinAccepts(u32 params) {
    const u8 type = params & 0xFF;
    return type == 0 || type == 1 || type == 0xFF;
}

constexpr DupSpec kDupSpecs[] = {
    {fpcNm_E_OC_e, 0x00FF0000, false, true, bokoblinAccepts},  // Bokoblin
    {fpcNm_E_BS_e, 0x00FF0000, false, true, nullptr},          // Stalkin
    {fpcNm_E_DN_e, 0xFF000000, false, true, nullptr},          // Lizalfos
    {fpcNm_E_MF_e, 0xFF000000, false, true, nullptr},          // Dynalfos
    {fpcNm_E_DD_e, 0xFF000000, false, true, nullptr},          // Dodongo
    {fpcNm_E_MM_e, 0x0000FF00, false, true, nullptr},          // Helmasaur (makes its own shell)
    {fpcNm_E_SF_e, 0, true, true, nullptr},                    // Stalfos
    {fpcNm_E_BA_e, 0xFF000000, false, false, nullptr},         // Keese
};

struct Job {
    enemy_sync::SpawnKey key;
    // The append is freed right after the original's create.
    fopAcM_prm_class prm;
    const DupSpec* spec;
    int seenExecuting = 0;
};

struct Extra {
    enemy_sync::SpawnKey key;
    fpc_ProcID originalId;
    const DupSpec* spec;
    fopAc_ac_c* actor = nullptr;  // set once created, valid until onActorDeleted
    cXyz spot;
    u32 params;
    s16 angleZ;
    Clock::time_point strayAt{};
    bool removed = false;
};

std::unordered_set<fpc_ProcID> s_placed;
std::deque<fpc_ProcID> s_placedOrder;
std::unordered_map<fpc_ProcID, Job> s_jobs;
std::unordered_map<fpc_ProcID, Extra> s_extras;
char s_stage[8] = {};

const DupSpec* findSpec(s16 procName, u32 params) {
    for (const DupSpec& s : kDupSpecs) {
        if (s.procName == procName && (s.accepts == nullptr || s.accepts(params))) {
            return &s;
        }
    }
    return nullptr;
}

uint32_t fnv(uint32_t h, const void* data, size_t n) {
    const auto* b = static_cast<const uint8_t*>(data);
    while (n-- != 0) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

uint32_t keyHash(const enemy_sync::SpawnKey& k) {
    const int32_t v[] = {k.procName, k.roomNo, static_cast<int32_t>(k.params), k.setId,
        static_cast<int32_t>(std::lround(k.home[0])), static_cast<int32_t>(std::lround(k.home[1])),
        static_cast<int32_t>(std::lround(k.home[2]))};
    return fnv(2166136261u, v, sizeof(v));
}

uint32_t nextRand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Around the original's placement: no wall in between, ground at about the same height, no water.
bool findSpawnPos(const cXyz& home, uint32_t seed, int k, bool needsGround, cXyz& out) {
    uint32_t rng = seed != 0 ? seed : 0x9E3779B9u;
    const s16 base = static_cast<s16>((nextRand(rng) & 0xFFFF) + k * 0x5555);
    const f32 lift = needsGround ? 60.0f : 0.0f;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const s16 angle = static_cast<s16>(base + attempt * 0x2000);
        const f32 r = 90.0f + static_cast<f32>(nextRand(rng) % 61);
        cXyz cand(home.x + cM_ssin(angle) * r, home.y, home.z + cM_scos(angle) * r);
        cXyz from(home.x, home.y + lift, home.z);
        cXyz to(cand.x, cand.y + lift, cand.z);
        dBgS_LinChk lin;
        lin.Set(&from, &to, nullptr);
        if (dComIfG_Bgsp().LineCross(&lin)) {
            continue;
        }
        if (needsGround) {
            cXyz probe(cand.x, cand.y + 100.0f, cand.z);
            dBgS_ObjGndChk gnd;
            gnd.SetPos(&probe);
            const f32 groundY = dComIfG_Bgsp().GroundCross(&gnd);
            if (groundY == -G_CM3D_F_INF || std::fabs(groundY - home.y) > 80.0f) {
                continue;
            }
            // Bokoblin's own water and lava test.
            dBgS_ObjGndChk_Spl spl;
            spl.SetPos(&probe);
            const f32 liquidY = dComIfG_Bgsp().GroundCross(&spl);
            if (liquidY != -G_CM3D_F_INF && liquidY >= groundY) {
                continue;
            }
            cand.y = groundY;
        }
        out = cand;
        return true;
    }
    return false;
}

int roomExtras(int8_t roomNo) {
    return static_cast<int>(std::count_if(s_extras.begin(), s_extras.end(),
        [&](const auto& e) { return e.second.key.roomNo == roomNo; }));
}

void spawn(const Job& job, fpc_ProcID originalId, fopAc_ac_c* original, int n) {
    const uint32_t hash = keyHash(job.key);
    const cXyz home = job.prm.base.position;
    // The room's layer: counted by room clears, deleted with the room.
    layer_class* layer = original->layer_tag.layer;
    // Numbered by attempt: a spot missing on one client never renumbers the next extra.
    for (int k = 1; k <= n; ++k) {
        if (roomExtras(job.key.roomNo) >= kMaxExtrasPerRoom) {
            break;
        }
        cXyz spot;
        if (!findSpawnPos(home, hash ^ (static_cast<uint32_t>(k) * 0x9E3779B9u), k,
                job.spec->needsGround, spot))
        {
            TwiliLog.info("[enemy] no spot for extra {} of {}", k, enemy_sync::keyText(job.key));
            continue;
        }
        fopAcM_prm_class* prm = fopAcM_CreateAppend();
        if (prm == nullptr) {
            break;
        }
        *prm = job.prm;
        prm->base.parameters = static_cast<u32>(job.prm.base.parameters) | job.spec->switchMask;
        csXyz angle = job.prm.base.angle;
        if (job.spec->switchInAngleZ) {
            angle.z = static_cast<s16>((angle.z & 0xFF00) | 0xFF);
        }
        prm->base.angle = angle;
        prm->base.position = spot;
        prm->base.setID = 0xFFFF;
        prm->parent_id = fpcM_ERROR_PROCESS_ID_e;
        layer_class* saved = fpcLy_CurrentLayer();
        fpcLy_SetCurrentLayer(layer);
        const fpc_ProcID id = fopAcM_Create(job.key.procName, NULL, prm);
        fpcLy_SetCurrentLayer(saved);
        if (id == fpcM_ERROR_PROCESS_ID_e) {
            cMl::free(prm);
            continue;
        }
        Extra e{};
        e.key = job.key;
        e.key.dup = static_cast<uint8_t>(k);
        e.originalId = originalId;
        e.spec = job.spec;
        e.spot = spot;
        e.params = static_cast<u32>(job.prm.base.parameters) | job.spec->switchMask;
        e.angleZ = angle.z;
        s_extras[id] = e;
    }
}

void decide(fpc_ProcID id, const Job& job) {
    const int pct = countPercent();
    const uint32_t hash = keyHash(job.key);
    int n = pct / 100 - 1 + (static_cast<int>(hash % 100) < pct % 100 ? 1 : 0);
    n = std::min(n, kMaxExtrasPerEnemy);
    if (n <= 0) {
        return;
    }
    if (mDoExt_getGameHeap()->getFreeSize() < kMinFreeHeap) {
        TwiliLog.warn("[enemy] game heap low: no extras for {}", enemy_sync::keyText(job.key));
        return;
    }
    if (fopAc_ac_c* original = fopAcM_SearchByID(id)) {
        spawn(job, id, original, n);
    }
}

// Removed like a defeat, so teammates lose theirs too.
void removeStuck(Extra& e, const char* why) {
    fopAc_ac_c* ac = e.actor;
    e.removed = true;
    TwiliLog.info("[enemy] removed extra {} of {}: {}", e.key.dup, enemy_sync::keyText(e.key), why);
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(ac->layer_tag.layer);
    fopAcM_createDisappear(ac, &ac->current.pos, 10, 0, 0xFF);
    fpcLy_SetCurrentLayer(saved);
    enemy_sync::markDefeated(ac);
    fopAcM_delete(ac);
}

void tickStuck() {
    const auto now = Clock::now();
    for (auto& [id, e] : s_extras) {
        if (e.actor == nullptr || e.removed || !fopAcM_IsExecuting(id) || enemy_sync::isDying(id)) {
            continue;
        }
        const cXyz& pos = e.actor->current.pos;
        if (pos.y < e.spot.y - kMaxFall) {
            removeStuck(e, "fell out of the room");
            continue;
        }
        const f32 dx = pos.x - e.spot.x, dy = pos.y - e.spot.y, dz = pos.z - e.spot.z;
        if (dx * dx + dy * dy + dz * dz <= kMaxStray * kMaxStray) {
            e.strayAt = {};
        } else if (e.strayAt == Clock::time_point{}) {
            e.strayAt = now;
        } else if (now - e.strayAt > kStrayTime) {
            removeStuck(e, "strayed from its spot");
        }
    }
}

}  // namespace

void onPlacedRequested(fpc_ProcID id) {
    if (id == fpcM_ERROR_PROCESS_ID_e || !s_placed.insert(id).second) {
        return;
    }
    // A request cancelled before its create never reaches onActorCreated.
    s_placedOrder.push_back(id);
    if (s_placedOrder.size() > kMaxPlaced) {
        s_placed.erase(s_placedOrder.front());
        s_placedOrder.pop_front();
    }
}

void onActorCreated(fopAc_ac_c* ac) {
    const fpc_ProcID id = fopAcM_GetID(ac);
    if (const auto it = s_extras.find(id); it != s_extras.end()) {
        it->second.actor = ac;
        return;
    }
    if (s_placed.erase(id) == 0) {
        return;
    }
    const fopAcM_prm_class* prm = fopAcM_GetAppend(ac);
    if (prm == nullptr) {
        return;
    }
    Job job{};
    job.spec = findSpec(fopAcM_GetName(ac), static_cast<u32>(prm->base.parameters));
    if (job.spec == nullptr || !enemy_sync::appendKey(ac, job.key)) {
        return;
    }
    job.prm = *prm;
    s_jobs[id] = job;
}

void onActorDeleted(fopAc_ac_c* ac) {
    const fpc_ProcID id = fopAcM_GetID(ac);
    s_jobs.erase(id);
    s_extras.erase(id);
    s_placed.erase(id);
}

bool extraKey(fpc_ProcID id, enemy_sync::SpawnKey& out) {
    const auto it = s_extras.find(id);
    if (it == s_extras.end()) {
        return false;
    }
    out = it->second.key;
    return true;
}

bool isExtra(fpc_ProcID id) {
    return s_extras.count(id) != 0;
}

fpc_ProcID extraOf(fpc_ProcID originalId, uint8_t dup) {
    for (const auto& [id, e] : s_extras) {
        if (e.originalId == originalId && e.key.dup == dup && fopAcM_IsExecuting(id)) {
            return id;
        }
    }
    return fpcM_ERROR_PROCESS_ID_e;
}

bool extraSpawn(fpc_ProcID id, ExtraSpawn& out) {
    const auto it = s_extras.find(id);
    if (it == s_extras.end()) {
        return false;
    }
    const Extra& e = it->second;
    out = {e.originalId, e.params, 0xFFFF, e.angleZ, e.spec->switchMask, e.spec->switchInAngleZ};
    return true;
}

void tick() {
    if (const char* stage = dComIfGp_getStartStageName();
        stage != nullptr && std::strncmp(stage, s_stage, sizeof(s_stage)) != 0)
    {
        std::strncpy(s_stage, stage, sizeof(s_stage) - 1);
        s_placed.clear();
        s_placedOrder.clear();
    }
    tickStuck();
    if (s_jobs.empty()) {
        return;
    }
    // Offline the room's percent is unknown: undecided, so a later join matches teammates.
    const bool ready = Session::active() && Session::instance().joined() &&
                       dComIfGp_getPlayer(0) != nullptr && !dComIfGp_event_runCheck() &&
                       !dComIfGp_isEnableNextStage();
    for (auto it = s_jobs.begin(); it != s_jobs.end();) {
        if (!fopAcM_IsExecuting(it->first)) {
            ++it;
            continue;
        }
        if (it->second.seenExecuting < kSettleUpdates) {
            ++it->second.seenExecuting;
            ++it;
            continue;
        }
        if (!ready) {
            ++it;
            continue;
        }
        // Once per original: a slider change applies when the area loads again.
        decide(it->first, it->second);
        it = s_jobs.erase(it);
    }
}

int countPercent() {
    if (!Session::active() || !Session::instance().joined()) {
        return 100;
    }
    return std::clamp(Session::instance().roomState().enemyCountMultiplier, 100, 300);
}

void shutdown() {
    // Live extras stay: they die with their room.
    s_placed.clear();
    s_placedOrder.clear();
    s_jobs.clear();
    s_extras.clear();
}

}  // namespace twili::enemy_count
