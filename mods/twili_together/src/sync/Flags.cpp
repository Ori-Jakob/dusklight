// Flag packets use dSv_info_c's numbering; memory flags of another stage go to its save slot.

#include "sync/WorldSyncState.hpp"

#include "core/SaveGate.hpp"
#include "story/Story.hpp"
#include "sync/LocalOnlyEventBits.hpp"
#include "sync/RemoteApplyGuard.hpp"

#include <cstring>
#include <string>

namespace twili::sync {
namespace detail {
namespace {

constexpr int kMemorySwitchEnd = dSv_info_c::MEMORY_SWITCH;
constexpr int kDanSwitchEnd = kMemorySwitchEnd + dSv_info_c::DAN_SWITCH;
constexpr int kZoneSwitchEnd = kDanSwitchEnd + dSv_info_c::ZONE_SWITCH + dSv_info_c::ONEZONE_SWITCH;

constexpr int kDanItemEnd = dSv_info_c::MEMORY_ITEM;
constexpr int kMemoryItemEnd = kDanItemEnd + dSv_info_c::DAN_ITEM;
constexpr int kZoneItemEnd = kMemoryItemEnd + dSv_info_c::ZONE_ITEM + dSv_info_c::ONEZONE_ITEM;

enum class Scope { Memory, Dan, Zone, Invalid };

Scope switchScope(int no) {
    if (no < 0)
        return Scope::Invalid;
    if (no < kMemorySwitchEnd)
        return Scope::Memory;
    if (no < kDanSwitchEnd)
        return Scope::Dan;
    if (no < kZoneSwitchEnd)
        return Scope::Zone;
    return Scope::Invalid;
}

Scope itemScope(int no) {
    if (no < 0)
        return Scope::Invalid;
    if (no < kDanItemEnd)
        return Scope::Dan;
    if (no < kMemoryItemEnd)
        return Scope::Memory;
    if (no < kZoneItemEnd)
        return Scope::Zone;
    return Scope::Invalid;
}

bool validSaveTbl(int saveTblNo) {
    return saveTblNo >= 0 && saveTblNo < dSv_save_c::STAGE_MAX;
}

// A room's zone slot exists only while it is loaded here.
bool zoneIsLoaded(const std::string& stageName, int roomNo) {
    const char* myStage = dComIfGp_getStartStageName();
    if (!myStage || std::strncmp(myStage, stageName.c_str(), 8) != 0) {
        return false;
    }
    if (roomNo < 0 || roomNo >= 64) {
        return false;
    }
    return dComIfGp_roomControl_getZoneNo(roomNo) >= 0;
}

// The live copy while that stage is loaded here, else its slot in the save.
dSv_memBit_c* memBitForSaveTbl(int saveTblNo) {
    if (!validSaveTbl(saveTblNo)) {
        return nullptr;
    }
    dSv_info_c* info = dComIfGs_getSaveInfo();
    if (saveTblNo == currentSaveTblNo()) {
        return &info->getMemory().getBit();
    }
    return &info->getSavedata().getSave(saveTblNo).getBit();
}

dSv_danBit_c* danBitForSaveTbl(int saveTblNo) {
    dSv_danBit_c& dan = dComIfGs_getSaveInfo()->getDan();
    if (saveTblNo < 0 || dan.mStageNo != saveTblNo) {
        return nullptr;
    }
    return &dan;
}

void sendFlag(const char* type, const char* category, int flagNo, int roomNo, int saveTblNo,
    bool persistent) {
    const char* stage = dComIfGp_getStartStageName();
    nlohmann::json packet = {
        {"type", type},
        {"flagNo", flagNo},
        {"roomNo", roomNo},
        {"stageName", std::string(stage ? stage : "")},
        {"saveTblNo", saveTblNo},
        {"quiet", !persistent},
    };
    if (category != nullptr) {
        packet["category"] = category;
    }
    stampWorldPacket(packet, persistent);
    send(std::move(packet));
}

}  // namespace

void noteMemorySwitch(int saveTblNo, int switchNo, bool set) {
    if (!validSaveTbl(saveTblNo) || switchScope(switchNo) != Scope::Memory) {
        return;
    }
    uint32_t& cleared = state().cleared.switches[saveTblNo][switchNo >> 5];
    const uint32_t mask = 1u << (switchNo & 0x1F);
    cleared = set ? (cleared & ~mask) : (cleared | mask);
}

void noteEventBit(uint16_t no, bool set) {
    uint8_t& cleared = state().cleared.eventBits[no >> 8];
    const uint8_t mask = static_cast<uint8_t>(no);
    cleared = static_cast<uint8_t>(set ? (cleared & ~mask) : (cleared | mask));
}

void handleSetFlag(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "SET_FLAG")) {
        return;
    }
    const std::string category = packet.value("category", std::string{});
    const int flagNo = packet.value("flagNo", -1);
    const int roomNo = packet.value("roomNo", -1);
    const int saveTblNo = packet.value("saveTblNo", -1);
    const std::string stageName = packet.value("stageName", std::string{});
    dSv_info_c* info = dComIfGs_getSaveInfo();

    RemoteApplyGuard guard;
    if (category == "SWITCH") {
        switch (switchScope(flagNo)) {
        case Scope::Memory:
            if (dSv_memBit_c* bit = memBitForSaveTbl(saveTblNo)) {
                bit->onSwitch(flagNo);
                noteMemorySwitch(saveTblNo, flagNo, true);
            }
            break;
        case Scope::Dan:
            if (dSv_danBit_c* dan = danBitForSaveTbl(saveTblNo)) {
                dan->onSwitch(flagNo - kMemorySwitchEnd);
            }
            break;
        case Scope::Zone:
            if (zoneIsLoaded(stageName, roomNo)) {
                info->onSwitch(flagNo, roomNo);
            }
            break;
        default:
            break;
        }
    } else if (category == "ITEM") {
        switch (itemScope(flagNo)) {
        case Scope::Dan:
            if (dSv_danBit_c* dan = danBitForSaveTbl(saveTblNo)) {
                dan->onItem(flagNo);
            }
            break;
        case Scope::Memory:
            if (dSv_memBit_c* bit = memBitForSaveTbl(saveTblNo)) {
                bit->onItem(flagNo - kDanItemEnd);
            }
            break;
        case Scope::Zone:
            if (zoneIsLoaded(stageName, roomNo)) {
                info->onItem(flagNo, roomNo);
            }
            break;
        default:
            break;
        }
    } else if (category == "TBOX") {
        if (flagNo >= 0 && flagNo < TBOX_MAX) {
            if (dSv_memBit_c* bit = memBitForSaveTbl(saveTblNo)) {
                bit->onTbox(flagNo);
            }
        }
    }
}

void handleUnsetFlag(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "UNSET_FLAG")) {
        return;
    }
    const int flagNo = packet.value("flagNo", -1);
    const int roomNo = packet.value("roomNo", -1);
    const int saveTblNo = packet.value("saveTblNo", -1);
    const std::string stageName = packet.value("stageName", std::string{});
    dSv_info_c* info = dComIfGs_getSaveInfo();

    RemoteApplyGuard guard;
    switch (switchScope(flagNo)) {
    case Scope::Memory:
        if (dSv_memBit_c* bit = memBitForSaveTbl(saveTblNo)) {
            bit->offSwitch(flagNo);
            noteMemorySwitch(saveTblNo, flagNo, false);
        }
        break;
    case Scope::Dan:
        if (dSv_danBit_c* dan = danBitForSaveTbl(saveTblNo)) {
            dan->offSwitch(flagNo - kMemorySwitchEnd);
        }
        break;
    case Scope::Zone:
        if (zoneIsLoaded(stageName, roomNo)) {
            info->offSwitch(flagNo, roomNo);
        }
        break;
    default:
        break;
    }
}

void handleSetEventBit(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "SET_EVENT_BIT")) {
        return;
    }
    const unsigned no = packet.value("no", 0u);
    if (no > 0xFFFF || isLocalOnlyEventBit(static_cast<uint16_t>(no))) {
        return;
    }
    RemoteApplyGuard guard;
    dComIfGs_getSaveInfo()->getEvent().onEventBit(static_cast<uint16_t>(no));
    noteEventBit(static_cast<uint16_t>(no), true);
}

void handleUnsetEventBit(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "UNSET_EVENT_BIT")) {
        return;
    }
    const unsigned no = packet.value("no", 0u);
    if (no > 0xFFFF || isLocalOnlyEventBit(static_cast<uint16_t>(no))) {
        return;
    }
    RemoteApplyGuard guard;
    dComIfGs_getSaveInfo()->getEvent().offEventBit(static_cast<uint16_t>(no));
    noteEventBit(static_cast<uint16_t>(no), false);
}

}  // namespace detail

using namespace detail;

void onSetFlag(const char* category, int flagNo, int roomNo) {
    if (RemoteApplyGuard::active() || !isSaveLoaded()) {
        return;
    }
    const std::string cat = category;
    Scope scope = Scope::Invalid;
    if (cat == "SWITCH") {
        scope = switchScope(flagNo);
    } else if (cat == "ITEM") {
        scope = itemScope(flagNo);
    } else if (cat == "TBOX") {
        scope = (flagNo >= 0 && flagNo < TBOX_MAX) ? Scope::Memory : Scope::Invalid;
    }
    if (scope == Scope::Invalid) {
        return;
    }
    const int saveTblNo = currentSaveTblNo();
    if (scope != Scope::Zone && saveTblNo < 0) {
        return;  // between stages
    }
    // Remembered even with sync off: turning it on starts a merge.
    if (scope == Scope::Memory && cat == "SWITCH") {
        noteMemorySwitch(saveTblNo, flagNo, true);
    }
    if (!enabled()) {
        return;
    }
    sendFlag("SET_FLAG", category, flagNo, roomNo, saveTblNo, scope != Scope::Zone);
}

void onUnsetFlag(int flagNo, int roomNo) {
    if (RemoteApplyGuard::active() || !isSaveLoaded()) {
        return;
    }
    const Scope scope = switchScope(flagNo);
    if (scope == Scope::Invalid) {
        return;
    }
    const int saveTblNo = currentSaveTblNo();
    if (scope != Scope::Zone && saveTblNo < 0) {
        return;
    }
    if (scope == Scope::Memory) {
        noteMemorySwitch(saveTblNo, flagNo, false);
    }
    if (!enabled()) {
        return;
    }
    sendFlag("UNSET_FLAG", nullptr, flagNo, roomNo, saveTblNo, scope != Scope::Zone);
}

void onEventBit(uint16_t no, bool set) {
    if (RemoteApplyGuard::active() || !isSaveLoaded() || isLocalOnlyEventBit(no)) {
        return;
    }
    noteEventBit(no, set);
    if (set) {
        // Story qualification: we wrote a synced story bit.
        story::noteLocalEventBit(no);
    }
    if (!enabled()) {
        return;
    }
    nlohmann::json packet = {
        {"type", set ? "SET_EVENT_BIT" : "UNSET_EVENT_BIT"},
        {"no", no},
    };
    stampWorldPacket(packet, true);
    send(std::move(packet));
}

}  // namespace twili::sync
