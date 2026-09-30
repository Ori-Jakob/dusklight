// Freestanding items a teammate took (field items with a save bit, heart pieces and containers, the
// Ordon Sword): they vanish here too, and a check reaches each of us once.

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"

#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_sword.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <fmt/format.h>
#include <mods/items.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace twili::sync {
namespace detail {
namespace {

constexpr int kNoBit = 0xFF;
constexpr int kMemoryItemBase = dSv_info_c::MEMORY_ITEM;
constexpr int kZoneItemBase = kMemoryItemBase + dSv_info_c::DAN_ITEM;
constexpr int kOneZoneItemBase = kZoneItemBase + dSv_info_c::ZONE_ITEM;
constexpr int kItemBitEnd = kOneZoneItemBase + dSv_info_c::ONEZONE_ITEM;
// fopAcM_createItemForBoss gives boss heart containers this memory bit.
constexpr int kBossItemBit = 0x9F;
// Ticks a busy item stays on the retry list (a boomerang or clawshot pull is shorter).
constexpr int kPendingTicks = 30 * 30;

enum class Kind { None, Item, Life, Sword };
enum class Use { Idle, Carried, Ours, Gone };

// Item bits a teammate set here (not already set), by scopeKey.
std::set<uint32_t> s_remoteBits;
// Checks a teammate's GIVE_ITEM granted us, and checks we took ourselves.
std::set<std::string> s_teamChecks;
std::set<std::string> s_ownChecks;

struct Pending {
    fpc_ProcID id;
    int ticksLeft;
};
std::vector<Pending> s_pending;

bool isZoneBit(int bit) {
    return bit >= kZoneItemBase;
}

uint32_t scopeKey(int saveTbl, int bit, int room) {
    if (isZoneBit(bit)) {
        return 3u << 24 | static_cast<uint32_t>(room & 0xFF) << 8 | static_cast<uint32_t>(bit);
    }
    const uint32_t scope = bit < kMemoryItemBase ? 2u : 1u;
    return scope << 24 | static_cast<uint32_t>(saveTbl & 0xFF) << 16 | static_cast<uint32_t>(bit);
}

Kind kindOf(fopAc_ac_c* actor) {
    switch (fopAcM_GetName(actor)) {
    case fpcNm_ITEM_e: return Kind::Item;
    case fpcNm_Obj_LifeContainer_e: return Kind::Life;
    case fpcNm_Obj_Sword_e: return Kind::Sword;
    default: return Kind::None;
    }
}

// One still being created is left alone: its create checks the bit again.
Kind liveKind(fopAc_ac_c* actor) {
    const Kind kind = kindOf(actor);
    return kind != Kind::None && fopAcM_IsExecuting(fopAcM_GetID(actor)) ? kind : Kind::None;
}

int bitOf(fopAc_ac_c* actor, Kind kind) {
    switch (kind) {
    case Kind::Item: return static_cast<int>(daItem_prm::getItemBitNo(static_cast<daItem_c*>(actor)));
    case Kind::Life:
        // A golden wolf's item goes by its event flag.
        return static_cast<daObjLife_c*>(actor)->mGoldenWolfItem ?
                   kNoBit :
                   static_cast<daObjLife_c*>(actor)->getSaveBitNo();
    case Kind::Sword: return static_cast<daObjSword_c*>(actor)->getItemBit();
    default: return kNoBit;
    }
}

Use useOf(fopAc_ac_c* actor, Kind kind) {
    const bool pulled = fopAcM_checkHookCarryNow(actor) != 0;
    switch (kind) {
    case Kind::Item: {
        auto* item = static_cast<daItem_c*>(actor);
        if (item->checkFlag(daItem_c::FLAG_DELETE_ITEM_e)) {
            return Use::Gone;
        }
        if (item->checkFlag(daItem_c::FLAG_INIT_GET_ITEM_e)) {
            return Use::Ours;
        }
        switch (item->getStatus()) {
        case daItem_c::STATUS_NORMAL_e:
        case daItem_c::STATUS_SW_ON_WAIT_e:
        case daItem_c::STATUS_BOOM_HIT_WAIT_e:
            return pulled ? Use::Carried : Use::Idle;
        case daItem_c::STATUS_ENEMY_CARRY_e:
        case daItem_c::STATUS_BOOMERANG_CARRY_e:
            return Use::Carried;
        default:
            return Use::Ours;
        }
    }
    case Kind::Life: {
        auto* life = static_cast<daObjLife_c*>(actor);
        if (life->chkStatus(daObjLife_c::STATUS_WAIT_e) ||
            life->chkStatus(daObjLife_c::STATUS_SW_ON_WAIT_e))
        {
            return pulled ? Use::Carried : Use::Idle;
        }
        // WAIT_2 is held by another actor (the fishing lure).
        if (life->chkStatus(daObjLife_c::STATUS_BOOMERANG_CARRY_e) ||
            life->chkStatus(daObjLife_c::STATUS_WAIT_2_e))
        {
            return Use::Carried;
        }
        return Use::Ours;
    }
    case Kind::Sword: {
        auto* sword = static_cast<daObjSword_c*>(actor);
        if (sword->mStatus != 0) {
            return Use::Ours;
        }
        return fopAcM_checkCarryNow(sword) ? Use::Carried : Use::Idle;
    }
    default:
        return Use::Gone;
    }
}

void removeActor(fopAc_ac_c* actor, Kind kind) {
    if (kind == Kind::Item) {
        static_cast<daItem_c*>(actor)->setFlag(daItem_c::FLAG_DELETE_ITEM_e);
    }
    fopAcM_delete(actor);
}

void addPending(fpc_ProcID id) {
    for (const Pending& p : s_pending) {
        if (p.id == id) {
            return;
        }
    }
    s_pending.push_back({id, kPendingTicks});
}

struct Match {
    int bit;
    int room;  // -1: any room
};

void* removeMatching(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const Kind kind = liveKind(actor);
    const auto* match = static_cast<const Match*>(data);
    if (kind == Kind::None || bitOf(actor, kind) != match->bit ||
        (match->room >= 0 && fopAcM_GetHomeRoomNo(actor) != match->room))
    {
        return nullptr;
    }
    switch (useOf(actor, kind)) {
    case Use::Idle:
        TwiliLog.info("[sync] item {} (bit {}) taken by a teammate: removed",
            fopAcM_GetParam(actor) & 0xFF, match->bit);
        removeActor(actor, kind);
        state().stats.freestandingRemoved++;
        break;
    case Use::Carried:
        addPending(fopAcM_GetID(actor));
        break;
    default:
        break;
    }
    return nullptr;
}

void removeTaken(int bit, int room) {
    Match match{bit, room};
    fopAcM_Search(&removeMatching, &match);
}

struct Busy {
    int bit;
    bool ours = false;
};

void* findOurs(void* proc, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(proc);
    const Kind kind = liveKind(actor);
    auto* busy = static_cast<Busy*>(data);
    if (kind != Kind::None && bitOf(actor, kind) == busy->bit && useOf(actor, kind) == Use::Ours) {
        busy->ours = true;
        return actor;
    }
    return nullptr;
}

bool sameStage(const std::string& stage) {
    const char* ours = dComIfGp_getStartStageName();
    return ours != nullptr && !stage.empty() && std::strncmp(ours, stage.c_str(), 8) == 0;
}

// "freestanding:<stage>:<bit>" and "boss:<stage>" with a dungeon or memory bit.
bool parseCheck(const std::string& check, std::string& stage, int& bit) {
    constexpr size_t kFreeLen = sizeof(ITEM_CHECK_FREESTANDING_PREFIX) - 1;
    constexpr size_t kBossLen = sizeof(ITEM_CHECK_BOSS_PREFIX) - 1;
    if (check.compare(0, kFreeLen, ITEM_CHECK_FREESTANDING_PREFIX) == 0) {
        const size_t colon = check.find(':', kFreeLen);
        if (colon == std::string::npos) {
            return false;
        }
        stage = check.substr(kFreeLen, colon - kFreeLen);
        char* end = nullptr;
        const long no = std::strtol(check.c_str() + colon + 1, &end, 10);
        if (end == check.c_str() + colon + 1 || *end != '\0') {
            return false;
        }
        bit = static_cast<int>(no);
    } else if (check.compare(0, kBossLen, ITEM_CHECK_BOSS_PREFIX) == 0) {
        stage = check.substr(kBossLen);
        bit = kBossItemBit;
    } else {
        return false;
    }
    return !stage.empty() && bit >= 0 && bit < kZoneItemBase;
}

std::string freestandingName(int bit) {
    const char* stage = dComIfGp_getStartStageName();
    return fmt::format("{}{}:{}", ITEM_CHECK_FREESTANDING_PREFIX, stage ? stage : "", bit);
}

std::string bossName() {
    const char* stage = dComIfGp_getStartStageName();
    return fmt::format("{}{}", ITEM_CHECK_BOSS_PREFIX, stage ? stage : "");
}

bool teamCheck(int bit) {
    if (s_teamChecks.empty()) {
        return false;
    }
    return s_teamChecks.count(freestandingName(bit)) != 0 ||
           (bit == kBossItemBit && s_teamChecks.count(bossName()) != 0);
}

}  // namespace

bool itemBitSet(int saveTbl, int bit, int room) {
    if (bit < 0 || bit >= kItemBitEnd) {
        return false;
    }
    if (bit < kMemoryItemBase) {
        const dSv_danBit_c* dan = danBitForSaveTbl(saveTbl);
        return dan != nullptr && ((dan->mItem[bit >> 5] >> (bit & 31)) & 1u) != 0;
    }
    if (bit < kZoneItemBase) {
        const dSv_memBit_c* mem = memBitForSaveTbl(saveTbl);
        const int no = bit - kMemoryItemBase;
        return mem != nullptr && ((static_cast<u32>(mem->mItem[0]) >> no) & 1u) != 0;
    }
    if (room < 0 || room >= 64) {
        return false;
    }
    const int zoneNo = dComIfGp_roomControl_getZoneNo(room);
    if (zoneNo < 0 || zoneNo >= dSv_info_c::ZONE_MAX) {
        return false;
    }
    const dSv_zoneBit_c& zone = dComIfGs_getSaveInfo()->getZone(zoneNo).getBit();
    return bit < kOneZoneItemBase ? zone.isItem(bit - kZoneItemBase) != FALSE :
                                    zone.isOneItem(bit - kOneZoneItemBase) != FALSE;
}

void onRemoteItemBit(int saveTbl, int bit, int room) {
    if (bit < 0 || bit >= kItemBitEnd) {
        return;
    }
    s_remoteBits.insert(scopeKey(saveTbl, bit, room));
    // Dungeon and zone bits only land while we are there; a memory bit may be another stage's.
    if (bit < kMemoryItemBase || isZoneBit(bit) || saveTbl == currentSaveTblNo()) {
        removeTaken(bit, isZoneBit(bit) ? room : -1);
    }
}

bool acceptTeamCheck(const std::string& check, int saveTbl) {
    std::string stage;
    int bit = kNoBit;
    if (!parseCheck(check, stage, bit)) {
        return true;
    }
    State& st = state();
    if (s_teamChecks.count(check) != 0 || s_ownChecks.count(check) != 0) {
        st.stats.grantsDeduped++;
        return false;
    }
    const bool here = sameStage(stage);
    if (here) {
        Busy busy{bit};
        fopAcM_Search(&findOurs, &busy);
        if (busy.ours) {
            s_ownChecks.insert(check);
            st.stats.grantsDeduped++;
            return false;
        }
    }
    // Set before and not by a teammate: we took it (maybe offline or before a restart).
    if (itemBitSet(saveTbl, bit, -1) && s_remoteBits.count(scopeKey(saveTbl, bit, -1)) == 0) {
        s_ownChecks.insert(check);
        st.stats.grantsDeduped++;
        return false;
    }
    s_teamChecks.insert(check);
    if (here) {
        removeTaken(bit, -1);
    }
    return true;
}

void noteOwnCheck(const char* check) {
    std::string stage;
    int bit = kNoBit;
    if (check != nullptr && parseCheck(check, stage, bit)) {
        s_ownChecks.insert(check);
    }
}

void tickFreestanding() {
    if (s_pending.empty()) {
        return;
    }
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        fopAc_ac_c* actor = fopAcM_SearchByID(p.id);
        const Kind kind = actor != nullptr ? liveKind(actor) : Kind::None;
        const Use use = kind != Kind::None ? useOf(actor, kind) : Use::Gone;
        if (use == Use::Carried && --p.ticksLeft > 0) {
            i++;
            continue;
        }
        if (use == Use::Idle) {
            TwiliLog.info("[sync] item {} taken by a teammate: removed once let go",
                fopAcM_GetParam(actor) & 0xFF);
            removeActor(actor, kind);
            state().stats.freestandingRemoved++;
        }
        s_pending[i] = s_pending.back();
        s_pending.pop_back();
    }
}

void clearFreestandingStage() {
    s_pending.clear();
    std::erase_if(s_remoteBits, [](uint32_t key) { return (key >> 24) == 3u; });
}

void resetFreestanding() {
    s_pending.clear();
    s_remoteBits.clear();
    s_teamChecks.clear();
    s_ownChecks.clear();
}

}  // namespace detail

using namespace detail;

bool blockTakenPickup(void* item) {
    auto* actor = static_cast<fopAc_ac_c*>(item);
    const Kind kind = liveKind(actor);
    const int bit = kind != Kind::None ? bitOf(actor, kind) : kNoBit;
    if (bit == kNoBit) {
        return false;
    }
    const int saveTbl = currentSaveTblNo();
    const int room = fopAcM_GetHomeRoomNo(actor);
    const bool taken = (itemBitSet(saveTbl, bit, room) &&
                           s_remoteBits.count(scopeKey(saveTbl, bit, room)) != 0) ||
                       (!isZoneBit(bit) && teamCheck(bit));
    if (!taken) {
        if (!isZoneBit(bit)) {
            s_ownChecks.insert(freestandingName(bit));
            if (bit == kBossItemBit) {
                s_ownChecks.insert(bossName());
            }
        }
        return false;
    }
    TwiliLog.info("[sync] item {} (bit {}) was taken by a teammate: not given again",
        fopAcM_GetParam(actor) & 0xFF, bit);
    state().stats.freestandingBlocked++;
    removeActor(actor, kind);
    return true;
}

}  // namespace twili::sync
