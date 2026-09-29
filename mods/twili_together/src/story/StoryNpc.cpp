#include "story/StoryNpc.hpp"

#include "core/Log.hpp"
#include "game/GameIdentity.hpp"
#include "sync/LocalOnlyEventBits.hpp"

#include "d/actor/d_a_npc.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace twili::story::npc {
namespace {

constexpr float kHomeTolerance = 50.0f;

// The daNpcT_c subclasses (include/d/actor/*.h) except npc_hoz, which overrides evtOrder.
constexpr int16_t kNpcTProfiles[] = {
    fpcNm_NPC_ARU_e, fpcNm_NPC_BESU_e, fpcNm_NPC_BOU_e, fpcNm_NPC_DOC_e, fpcNm_NPC_FAIRY_e,
    fpcNm_NPC_FAIRY_SEIREI_e, fpcNm_NPC_GND_e, fpcNm_NPC_HANJO_e, fpcNm_NPC_JAGAR_e,
    fpcNm_NPC_KAKASHI_e, fpcNm_NPC_KKRI_e, fpcNm_NPC_KNJ_e, fpcNm_NPC_KOLIN_e, fpcNm_NPC_KOLINB_e,
    fpcNm_NPC_KYURY_e, fpcNm_NPC_LEN_e, fpcNm_NPC_LUD_e, fpcNm_NPC_MIDP_e, fpcNm_NPC_MOI_e,
    fpcNm_NPC_PACHI_BESU_e, fpcNm_NPC_PACHI_MARO_e, fpcNm_NPC_PACHI_TARO_e, fpcNm_NPC_POST_e,
    fpcNm_NPC_POUYA_e, fpcNm_NPC_RACA_e, fpcNm_NPC_SARU_e, fpcNm_NPC_SEIB_e, fpcNm_NPC_SEIC_e,
    fpcNm_NPC_SEID_e, fpcNm_NPC_SEIREI_e, fpcNm_NPC_SHAMAN_e, fpcNm_NPC_SOLA_e, fpcNm_NPC_TARO_e,
    fpcNm_NPC_TKJ_e, fpcNm_NPC_TOBY_e, fpcNm_NPC_URI_e, fpcNm_NPC_YAMID_e, fpcNm_NPC_YAMIS_e,
    fpcNm_NPC_YAMIT_e, fpcNm_NPC_YELIA_e, fpcNm_NPC_YKM_e, fpcNm_NPC_YKW_e, fpcNm_NPC_ZANB_e,
    fpcNm_NPC_ZANT_e, fpcNm_NPC_ZELR_e, fpcNm_NPC_ZELRO_e, fpcNm_NPC_ZELDA_e, fpcNm_OBJ_SEKIZOA_e,
    fpcNm_PERU_e,
};

struct Allowed {
    int16_t profile;
    const char* event;
};

// Grows one entry at a time, each once a scenario shows it plays on both screens.
constexpr std::array<Allowed, 0> kNpcJoinable{};

std::vector<std::pair<int16_t, std::string>> s_testAllowed;
bool s_perturb = false;

struct Ordered {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    int index = 0;
};
std::array<Ordered, 8> s_ordered{};
size_t s_orderedNext = 0;

struct Pending {
    fpc_ProcID id = fpcM_ERROR_PROCESS_ID_e;
    int index = 0;
};
Pending s_pending;

struct Find {
    const SpawnKey* key;
    daNpcT_c* found;
};

void* findSpawn(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    auto* find = static_cast<Find*>(data);
    if (fopAcM_GetName(actor) != find->key->procName || !isNpcT(find->key->procName)) {
        return nullptr;
    }
    if (!sameSpawn(spawnKeyOf(actor), *find->key, kHomeTolerance) ||
        actor->eventInfo.checkCommandDemoAccrpt())
    {
        return nullptr;
    }
    find->found = static_cast<daNpcT_c*>(actor);
    return actor;
}

}  // namespace

bool isNpcT(int16_t profile) {
    return std::find(std::begin(kNpcTProfiles), std::end(kNpcTProfiles), profile) !=
           std::end(kNpcTProfiles);
}

bool joinable(int16_t profile, const std::string& event) {
    for (const Allowed& a : kNpcJoinable) {
        if (a.profile == profile && event == a.event) {
            return true;
        }
    }
    return std::find(s_testAllowed.begin(), s_testAllowed.end(), std::make_pair(profile, event)) !=
           s_testAllowed.end();
}

bool joinsOff() {
    return game_identity::current().kind == "randomizer";
}

uint64_t storyDigest() {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](uint8_t b) {
        hash ^= b;
        hash *= 0x100000001b3ull;
    };
    const dSv_event_c& ev = dComIfGs_getSaveInfo()->getEvent();
    for (int i = 0; i < 256; i++) {
        // 0xF1 on hold counters (fishing, rupee totals, goats), not flags.
        const uint8_t b = i >= 0xF1 ? 0 : ev.mEvent[i];
        mix(static_cast<uint8_t>(b & ~sync::localOnlyEventMask(i)));
    }
    const dSv_memBit_c& mem = dComIfGs_getSaveInfo()->getMemory().getBit();
    for (int w = 0; w < 4; w++) {
        const uint32_t word = static_cast<uint32_t>(mem.mSwitch[w]);
        for (int s = 0; s < 32; s += 8) {
            mix(static_cast<uint8_t>(word >> s));
        }
    }
    if (s_perturb) {
        mix(0x5A);
    }
    return hash;
}

void beforeEvtOrder(daNpcT_c* npc) {
    const fpc_ProcID id = fopAcM_GetID(npc);
    if (s_pending.index > 0 && s_pending.id == id) {
        TwiliLog.info("[story] NPC 0x{:X} orders its event entry {} '{}'", fopAcM_GetName(npc),
            s_pending.index, npc->mpEvtData[s_pending.index].eventName);
        npc->mEvtNo = static_cast<u16>(s_pending.index);
        s_pending = {};
    }
    if (npc->mEvtNo == 0 || npc->mpEvtData == nullptr ||
        npc->mpEvtData[npc->mEvtNo].eventName[0] == '\0')
    {
        return;
    }
    for (Ordered& o : s_ordered) {
        if (o.id == id) {
            o.index = npc->mEvtNo;
            return;
        }
    }
    s_ordered[s_orderedNext] = {id, npc->mEvtNo};
    s_orderedNext = (s_orderedNext + 1) % s_ordered.size();
}

int orderedIndex(const fopAc_ac_c* actor) {
    if (actor == nullptr) {
        return -1;
    }
    const fpc_ProcID id = fopAcM_GetID(actor);
    for (const Ordered& o : s_ordered) {
        if (o.id == id) {
            return o.index;
        }
    }
    return -1;
}

std::string eventName(const daNpcT_c* npc, int index) {
    if (npc == nullptr || npc->mpEvtData == nullptr || index <= 0) {
        return {};
    }
    return npc->mpEvtData[index].eventName;
}

daNpcT_c* findNpc(const SpawnKey& key) {
    Find find{&key, nullptr};
    fopAcM_Search(&findSpawn, &find);
    return find.found;
}

void placeOrder(daNpcT_c* npc, int index) {
    s_pending = npc != nullptr && index > 0 ? Pending{fopAcM_GetID(npc), index} : Pending{};
}

bool orderPending() {
    return s_pending.index > 0;
}

#if TWILI_ENABLE_AUTOTEST
void allowForTest(int16_t profile, const std::string& event) {
    s_testAllowed.emplace_back(profile, event);
}

void perturbDigestForTest(bool on) {
    s_perturb = on;
}
#endif

}  // namespace twili::story::npc
