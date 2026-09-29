#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_save.h"
#include "f_pc/f_pc_manager.h"

#include <array>

namespace twili::hooks {

DEFINE_HOOK(&dSv_memBit_c::onTbox, MemBitOnTbox);
DEFINE_HOOK(&dSv_memBit_c::onSwitch, MemBitOnSwitch);
DEFINE_HOOK(&dSv_memBit_c::offSwitch, MemBitOffSwitch);
DEFINE_HOOK(&dSv_memBit_c::onItem, MemBitOnItem);
DEFINE_HOOK(&dSv_event_c::onEventBit, EventOnBit);
DEFINE_HOOK(&dSv_event_c::offEventBit, EventOffBit);
DEFINE_HOOK(&dSv_danBit_c::onSwitch, DanBitOnSwitch);
DEFINE_HOOK(&dSv_danBit_c::offSwitch, DanBitOffSwitch);
DEFINE_HOOK(&dSv_danBit_c::onItem, DanBitOnItem);
DEFINE_HOOK(&dSv_info_c::onSwitch, InfoOnSwitch);
DEFINE_HOOK(&dSv_info_c::offSwitch, InfoOffSwitch);
DEFINE_HOOK(&dSv_info_c::onItem, InfoOnItem);
DEFINE_HOOK(&dSv_info_c::memory_to_card, SaveMemoryToCard);
DEFINE_HOOK(&execItemGet, ItemExecItemGet);
DEFINE_HOOK(&fpcM_Management, ProcManagement);

namespace {

constexpr int kDanSwitchBase = dSv_info_c::MEMORY_SWITCH;
constexpr int kZoneSwitchBase = kDanSwitchBase + dSv_info_c::DAN_SWITCH;
constexpr int kOneZoneSwitchBase = kZoneSwitchBase + dSv_info_c::ZONE_SWITCH;
constexpr int kZoneSwitchEnd = kOneZoneSwitchBase + dSv_info_c::ONEZONE_SWITCH;
constexpr int kMemoryItemBase = dSv_info_c::MEMORY_ITEM;
constexpr int kZoneItemBase = kMemoryItemBase + dSv_info_c::DAN_ITEM;
constexpr int kOneZoneItemBase = kZoneItemBase + dSv_info_c::ZONE_ITEM;
constexpr int kZoneItemEnd = kOneZoneItemBase + dSv_info_c::ONEZONE_ITEM;

// Old bit per open call of one target (-1: not ours to report); the post-hook decides.
class Snapshots {
public:
    void push(int value) {
        if (mDepth < mValues.size()) {
            mValues[mDepth] = static_cast<int8_t>(value);
        }
        mDepth++;
    }
    int pop() {
        if (mDepth == 0) {
            return -1;
        }
        mDepth--;
        return mDepth < mValues.size() ? mValues[mDepth] : -1;
    }

private:
    std::array<int8_t, 8> mValues{};
    size_t mDepth = 0;
};

bool observing() {
    return Session::active() && !sync::RemoteApplyGuard::active() && isSaveLoaded();
}

dSv_info_c& info() {
    return *dComIfGs_getSaveInfo();
}

// Raw reads: other mods hook the is* accessors.
int rawBit(u32 word, int no) {
    return static_cast<int>((word >> (no & 31)) & 1u);
}

int memBitOld(dSv_memBit_c* bit, int kind, int no) {
    if (!observing() || bit != &info().getMemory().getBit() || no < 0) {
        return -1;
    }
    switch (kind) {
    case 0:
        return no < TBOX_MAX ? rawBit(static_cast<u32>(bit->mTbox[no >> 5]), no) : -1;
    case 1:
        return no < 128 ? rawBit(static_cast<u32>(bit->mSwitch[no >> 5]), no) : -1;
    default:
        return no < 32 ? rawBit(static_cast<u32>(bit->mItem[0]), no) : -1;
    }
}

int danBitOld(dSv_danBit_c* dan, bool item, int no) {
    if (!observing() || dan != &info().getDan() || no < 0) {
        return -1;
    }
    if (item) {
        return no < ITEM_MAX_DAN ? rawBit(dan->mItem[no >> 5], no) : -1;
    }
    return no < 64 ? rawBit(dan->mSwitch[no >> 5], no) : -1;
}

int eventOld(dSv_event_c* event, uint16_t no) {
    if (!observing() || event != &info().getEvent()) {
        return -1;
    }
    return (event->mEvent[no >> 8] & static_cast<uint8_t>(no)) != 0 ? 1 : 0;
}

// Zone switches and items only; memory and dungeon ones report from their bit structs.
int zoneOld(dSv_info_c* self, bool item, int no, int room) {
    const int base = item ? kZoneItemBase : kZoneSwitchBase;
    const int end = item ? kZoneItemEnd : kZoneSwitchEnd;
    if (!observing() || self != &info() || no < base || no >= end || room < 0 || room >= 64) {
        return -1;
    }
    const int zoneNo = dComIfGp_roomControl_getZoneNo(room);
    if (zoneNo < 0 || zoneNo >= dSv_info_c::ZONE_MAX) {
        return -1;
    }
    const dSv_zoneBit_c& bit = self->getZone(zoneNo).getBit();
    if (item) {
        return no < kOneZoneItemBase ? bit.isItem(no - kZoneItemBase) != FALSE :
                                       bit.isOneItem(no - kOneZoneItemBase) != FALSE;
    }
    return no < kOneZoneSwitchBase ? bit.isSwitch(no - kZoneSwitchBase) != FALSE :
                                     bit.isOneSwitch(no - kOneZoneSwitchBase) != FALSE;
}

Snapshots s_tbox, s_memOn, s_memOff, s_memItem, s_evOn, s_evOff, s_danOn, s_danOff, s_danItem,
    s_infoOn, s_infoOff, s_infoItem;

HookAction onTboxPre(ModContext*, void* args, void*, void*) {
    s_tbox.push(memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 0, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void onTboxPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    // A tear's bit travels in its LIGHT_DROP.
    if (s_tbox.pop() == 0 && memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 0, no) == 1 &&
        !sync::insideDropGet())
    {
        sync::onSetFlag("TBOX", no, 0);
    }
}

HookAction onMemSwitchPre(ModContext*, void* args, void*, void*) {
    s_memOn.push(memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 1, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void onMemSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_memOn.pop() == 0 && memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 1, no) == 1) {
        sync::onSetFlag("SWITCH", no, -1);
    }
}

HookAction offMemSwitchPre(ModContext*, void* args, void*, void*) {
    s_memOff.push(memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 1, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void offMemSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_memOff.pop() == 1 && memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 1, no) == 0) {
        sync::onUnsetFlag(no, -1);
    }
}

HookAction onMemItemPre(ModContext*, void* args, void*, void*) {
    s_memItem.push(memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 2, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void onMemItemPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_memItem.pop() == 0 && memBitOld(mods::arg<dSv_memBit_c*>(args, 0), 2, no) == 1) {
        sync::onSetFlag("ITEM", no + kMemoryItemBase, -1);
    }
}

HookAction onEventPre(ModContext*, void* args, void*, void*) {
    s_evOn.push(eventOld(mods::arg<dSv_event_c*>(args, 0), mods::arg<u16>(args, 1)));
    return HOOK_CONTINUE;
}

void onEventPost(ModContext*, void* args, void*, void*) {
    const u16 no = mods::arg<u16>(args, 1);
    if (s_evOn.pop() == 0 && eventOld(mods::arg<dSv_event_c*>(args, 0), no) == 1) {
        sync::onEventBit(no, true);
    }
}

HookAction offEventPre(ModContext*, void* args, void*, void*) {
    s_evOff.push(eventOld(mods::arg<dSv_event_c*>(args, 0), mods::arg<u16>(args, 1)));
    return HOOK_CONTINUE;
}

void offEventPost(ModContext*, void* args, void*, void*) {
    const u16 no = mods::arg<u16>(args, 1);
    if (s_evOff.pop() == 1 && eventOld(mods::arg<dSv_event_c*>(args, 0), no) == 0) {
        sync::onEventBit(no, false);
    }
}

HookAction onDanSwitchPre(ModContext*, void* args, void*, void*) {
    s_danOn.push(danBitOld(mods::arg<dSv_danBit_c*>(args, 0), false, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void onDanSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_danOn.pop() == 0 && danBitOld(mods::arg<dSv_danBit_c*>(args, 0), false, no) == 1) {
        sync::onSetFlag("SWITCH", no + kDanSwitchBase, -1);
    }
}

HookAction offDanSwitchPre(ModContext*, void* args, void*, void*) {
    s_danOff.push(danBitOld(mods::arg<dSv_danBit_c*>(args, 0), false, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void offDanSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_danOff.pop() == 1 && danBitOld(mods::arg<dSv_danBit_c*>(args, 0), false, no) == 0) {
        sync::onUnsetFlag(no + kDanSwitchBase, -1);
    }
}

HookAction onDanItemPre(ModContext*, void* args, void*, void*) {
    s_danItem.push(danBitOld(mods::arg<dSv_danBit_c*>(args, 0), true, mods::arg<int>(args, 1)));
    return HOOK_CONTINUE;
}

void onDanItemPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    if (s_danItem.pop() == 0 && danBitOld(mods::arg<dSv_danBit_c*>(args, 0), true, no) == 1) {
        sync::onSetFlag("ITEM", no, -1);
    }
}

HookAction onInfoSwitchPre(ModContext*, void* args, void*, void*) {
    s_infoOn.push(zoneOld(
        mods::arg<dSv_info_c*>(args, 0), false, mods::arg<int>(args, 1), mods::arg<int>(args, 2)));
    return HOOK_CONTINUE;
}

void onInfoSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    if (s_infoOn.pop() == 0 && zoneOld(mods::arg<dSv_info_c*>(args, 0), false, no, room) == 1) {
        sync::onSetFlag("SWITCH", no, room);
    }
}

HookAction offInfoSwitchPre(ModContext*, void* args, void*, void*) {
    s_infoOff.push(zoneOld(
        mods::arg<dSv_info_c*>(args, 0), false, mods::arg<int>(args, 1), mods::arg<int>(args, 2)));
    return HOOK_CONTINUE;
}

void offInfoSwitchPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    if (s_infoOff.pop() == 1 && zoneOld(mods::arg<dSv_info_c*>(args, 0), false, no, room) == 0) {
        sync::onUnsetFlag(no, room);
    }
}

HookAction onInfoItemPre(ModContext*, void* args, void*, void*) {
    s_infoItem.push(zoneOld(
        mods::arg<dSv_info_c*>(args, 0), true, mods::arg<int>(args, 1), mods::arg<int>(args, 2)));
    return HOOK_CONTINUE;
}

void onInfoItemPost(ModContext*, void* args, void*, void*) {
    const int no = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    if (s_infoItem.pop() == 0 && zoneOld(mods::arg<dSv_info_c*>(args, 0), true, no, room) == 1) {
        sync::onSetFlag("ITEM", no, room);
    }
}

// Also covers autosave, which SaveService would not report.
void onMemoryToCardPost(ModContext*, void*, void* retval, void*) {
    if (retval != nullptr && *static_cast<int*>(retval) == 0 && observing()) {
        sync::onSaveWritten();
    }
}

HookAction onExecItemGetPre(ModContext*, void* args, void*, void*) {
    sync::beforeItemGet(mods::arg<u8>(args, 0));
    return HOOK_CONTINUE;
}

void onExecItemGetPost(ModContext*, void* args, void*, void*) {
    sync::afterItemGet(mods::arg<u8>(args, 0));
}

HookAction onProcManagementPre(ModContext*, void*, void*, void*) {
    perf::count(perf::Target::Management);
    sync::closeGrantWindow();
    return HOOK_CONTINUE;
}

template <class Entry>
ModResult addObserver(HookPreFn pre, HookPostFn post, const char* what, std::string& error) {
    ModResult result = addPre<Entry>(pre, kObserve, what, error);
    if (result == MOD_OK) {
        result = addPost<Entry>(post, kDefault, what, error);
    }
    return result;
}

}  // namespace

ModResult installSync(std::string& error) {
    const ModResult results[] = {
        addObserver<MemBitOnTbox>(onTboxPre, onTboxPost, "dSv_memBit_c::onTbox", error),
        addObserver<MemBitOnSwitch>(
            onMemSwitchPre, onMemSwitchPost, "dSv_memBit_c::onSwitch", error),
        addObserver<MemBitOffSwitch>(
            offMemSwitchPre, offMemSwitchPost, "dSv_memBit_c::offSwitch", error),
        addObserver<MemBitOnItem>(onMemItemPre, onMemItemPost, "dSv_memBit_c::onItem", error),
        addObserver<EventOnBit>(onEventPre, onEventPost, "dSv_event_c::onEventBit", error),
        addObserver<EventOffBit>(offEventPre, offEventPost, "dSv_event_c::offEventBit", error),
        addObserver<DanBitOnSwitch>(
            onDanSwitchPre, onDanSwitchPost, "dSv_danBit_c::onSwitch", error),
        addObserver<DanBitOffSwitch>(
            offDanSwitchPre, offDanSwitchPost, "dSv_danBit_c::offSwitch", error),
        addObserver<DanBitOnItem>(onDanItemPre, onDanItemPost, "dSv_danBit_c::onItem", error),
        addObserver<InfoOnSwitch>(onInfoSwitchPre, onInfoSwitchPost, "dSv_info_c::onSwitch", error),
        addObserver<InfoOffSwitch>(
            offInfoSwitchPre, offInfoSwitchPost, "dSv_info_c::offSwitch", error),
        addObserver<InfoOnItem>(onInfoItemPre, onInfoItemPost, "dSv_info_c::onItem", error),
        addPost<SaveMemoryToCard>(onMemoryToCardPost, kDefault, "memory_to_card", error),
        addObserver<ItemExecItemGet>(onExecItemGetPre, onExecItemGetPost, "execItemGet", error),
        addPre<ProcManagement>(onProcManagementPre, kObserve, "fpcM_Management", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
