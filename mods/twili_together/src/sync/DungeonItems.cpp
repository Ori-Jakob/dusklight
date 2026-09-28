// UPDATE_DUNGEON_ITEMS: live small-key deltas (they commute) and new dungeon-item bits.

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "ui/ItemToasts.hpp"

#include "d/d_item_data.h"

#include <algorithm>

namespace twili::sync::detail {
namespace {

void sendUpdateDungeonItems(int saveTblNo, int keyDelta, uint8_t dungeonItemBits) {
    nlohmann::json packet = {
        {"type", "UPDATE_DUNGEON_ITEMS"},
        {"saveTblNo", saveTblNo},
        {"keyDelta", keyDelta},
        {"dungeonItemBits", static_cast<unsigned>(dungeonItemBits)},
    };
    stampWorldPacket(packet, true);
    send(std::move(packet));
}

}  // namespace

void shiftDungeonItemBaseline(int keyNumBefore, uint8_t dungeonItemsBefore) {
    State& st = state();
    const int saveTbl = currentSaveTblNo();
    if (!st.dungeonBaselineValid || saveTbl < 0 || st.dungeonBaselineSaveTbl != saveTbl) {
        return;  // the next tick takes a fresh baseline, which includes the write
    }
    const dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    st.dungeonBaselineKeyNum += bit.mKeyNum - keyNumBefore;
    st.dungeonBaselineItems |= static_cast<uint8_t>(bit.mDungeonItem & ~dungeonItemsBefore);
}

void tickDungeonItemTracking() {
    State& st = state();
    const int saveTbl = currentSaveTblNo();
    if (!isSaveLoaded() || saveTbl < 0) {
        st.dungeonBaselineValid = false;
        return;
    }
    const dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    const int keyNum = bit.mKeyNum;
    const uint8_t items = bit.mDungeonItem;
    if (!st.dungeonBaselineValid || st.dungeonBaselineSaveTbl != saveTbl) {
        st.dungeonBaselineValid = true;
        st.dungeonBaselineSaveTbl = saveTbl;
        st.dungeonBaselineKeyNum = keyNum;
        st.dungeonBaselineItems = items;
        return;
    }
    const int keyDelta = keyNum - st.dungeonBaselineKeyNum;
    const uint8_t newBits = static_cast<uint8_t>(items & ~st.dungeonBaselineItems);
    if (keyDelta != 0) {
        // A snapshot that predates this change must not overwrite it, even with sync off.
        st.keyCountAdoptable &= ~(1u << saveTbl);
    }
    if ((keyDelta != 0 || newBits != 0) && enabled()) {
        sendUpdateDungeonItems(saveTbl, keyDelta, newBits);
    }
    st.dungeonBaselineKeyNum = keyNum;
    st.dungeonBaselineItems = items;
}

void handleUpdateDungeonItems(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "UPDATE_DUNGEON_ITEMS")) {
        return;
    }
    const int saveTbl = packet.value("saveTblNo", -1);
    if (saveTbl < 0 || saveTbl >= dSv_save_c::STAGE_MAX) {
        return;
    }
    // A real delta is a few keys; the clamp only stops a forged one from overflowing.
    const int keyDelta = std::clamp(packet.value("keyDelta", 0), -255, 255);
    const uint8_t bits = static_cast<uint8_t>(packet.value("dungeonItemBits", 0u) & 0xFF);

    dSv_info_c* info = dComIfGs_getSaveInfo();
    const bool live = saveTbl == currentSaveTblNo();
    dSv_memBit_c& bit =
        live ? info->getMemory().getBit() : info->getSavedata().getSave(saveTbl).getBit();
    const int keyNumBefore = bit.mKeyNum;
    const uint8_t dungeonItemsBefore = bit.mDungeonItem;
    bit.mKeyNum = static_cast<u8>(std::clamp(keyNumBefore + keyDelta, 0, 255));
    bit.mDungeonItem |= bits;
    if (live) {
        shiftDungeonItemBaseline(keyNumBefore, dungeonItemsBefore);
    }
    TwiliLog.info("[sync] dungeon items from client {}: stage table {} keys {:+d} bits 0x{:02X}",
        packet.value("clientId", 0u), saveTbl, keyDelta, bits);

    // A negative delta is a door; boss-beaten and heart-container bits are no pickup.
    const auto source =
        packet.value("fromQueue", false) ? item_toasts::Source::Replay : item_toasts::Source::Live;
    const item_toasts::Sender who = item_toasts::senderOf(Session::instance(), packet);
    const uint8_t gained = bits & ~dungeonItemsBefore;
    if (keyDelta > 0) {
        item_toasts::noteItem(who, dItemNo_SMALL_KEY_e, keyDelta, saveTbl, source, true);
    }
    if ((gained & (1 << dSv_memBit_c::MAP)) != 0) {
        item_toasts::noteItem(who, dItemNo_MAP_e, 1, saveTbl, source, true);
    }
    if ((gained & (1 << dSv_memBit_c::COMPASS)) != 0) {
        item_toasts::noteItem(who, dItemNo_COMPUS_e, 1, saveTbl, source, true);
    }
    if ((gained & (1 << dSv_memBit_c::BOSS_KEY)) != 0) {
        item_toasts::noteItem(who, dItemNo_BOSS_KEY_e, 1, saveTbl, source, true);
    }
}

}  // namespace twili::sync::detail
