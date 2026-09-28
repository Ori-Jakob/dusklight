// UPDATE_WORLD_STATE: the whole save as layout-checked raw blobs, merged so nothing regresses.

#include "sync/WorldSyncState.hpp"

#include "core/Base64.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/LocalOnlyEventBits.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/ItemToasts.hpp"

#include "d/d_item_data.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace twili::sync {
namespace detail {
namespace {

constexpr const char* kFormat = "dsv1";

constexpr int kBottleSlotFirst = SLOT_11;
constexpr int kBottleSlotCount = dSv_player_item_c::BOTTLE_MAX;
constexpr int kBombBagSlotFirst = SLOT_15;
constexpr int kBombBagSlotCount = dSv_player_item_c::BOMB_BAG_MAX;

template <typename T>
T maxOf(T a, T b) {
    return a > b ? a : b;
}

std::string encodeBytes(const void* data, size_t size) {
    return base64::encode(std::span<const uint8_t>(static_cast<const uint8_t*>(data), size));
}

bool decodeBytes(const nlohmann::json& packet, const char* key, void* out, size_t size) {
    const auto it = packet.find(key);
    if (it == packet.end() || !it->is_string()) {
        return false;
    }
    std::vector<uint8_t> decoded;
    if (!base64::decode(it->get_ref<const std::string&>(), decoded) || decoded.size() != size) {
        return false;
    }
    std::memcpy(out, decoded.data(), size);
    return true;
}

bool isBottleSlot(int slot) {
    return slot >= kBottleSlotFirst && slot < kBottleSlotFirst + kBottleSlotCount;
}

bool isBombBagSlot(int slot) {
    return slot >= kBombBagSlotFirst && slot < kBombBagSlotFirst + kBombBagSlotCount;
}

// No chest, switch, item or dungeon-item bit and no keys.
bool isUntouchedStage(const dSv_memBit_c& bit) {
    return static_cast<u32>(bit.mTbox[0]) == 0u && static_cast<u32>(bit.mTbox[1]) == 0u &&
           static_cast<u32>(bit.mSwitch[0]) == 0u && static_cast<u32>(bit.mSwitch[1]) == 0u &&
           static_cast<u32>(bit.mSwitch[2]) == 0u && static_cast<u32>(bit.mSwitch[3]) == 0u &&
           static_cast<u32>(bit.mItem[0]) == 0u && bit.mKeyNum == 0 && bit.mDungeonItem == 0;
}

// The current stage is read from the live memory, which is ahead of its save-table slot.
uint32_t untouchedStageMask(dSv_info_c& info, int currentSaveTbl) {
    static_assert(dSv_save_c::STAGE_MAX <= 32);
    uint32_t mask = 0;
    for (int s = 0; s < dSv_save_c::STAGE_MAX; s++) {
        const dSv_memBit_c& bit = s == currentSaveTbl ? info.getMemory().getBit() :
                                                        info.getSavedata().getSave(s).getBit();
        if (isUntouchedStage(bit)) {
            mask |= 1u << s;
        }
    }
    return mask;
}

// Keys travel as deltas; only a stage untouched at catch-up time takes the snapshot's count.
void mergeMemBit(dSv_memBit_c& local, const dSv_memBit_c& remote, bool adoptKeyNum,
    const uint32_t (&clearedSwitches)[4]) {
    if (adoptKeyNum) {
        local.mKeyNum = remote.mKeyNum;
    }
    for (int i = 0; i < 2; i++) {
        local.mTbox[i] = static_cast<u32>(local.mTbox[i]) | static_cast<u32>(remote.mTbox[i]);
    }
    for (int i = 0; i < 4; i++) {
        local.mSwitch[i] = static_cast<u32>(local.mSwitch[i]) |
                           (static_cast<u32>(remote.mSwitch[i]) & ~clearedSwitches[i]);
    }
    local.mItem[0] = static_cast<u32>(local.mItem[0]) | static_cast<u32>(remote.mItem[0]);
    local.mDungeonItem |= remote.mDungeonItem;
}

// Bits of mEvent[byte] that hold a counter (setEventReg), not flags: OR-ing counters is wrong.
uint8_t eventRegisterMask(int byte) {
    switch (byte) {
    case 0xF1:  // SP_LURE__USE_COUNT
    case 0xFF:  // EREG_000 (runaway goats)
        return 0x1F;
    case 0xF2:  // fishing records
    case 0xF3:
    case 0xF4:
    case 0xF5:
        return 0x7F;
    case 0xF6:  // KORO2_LEVEL
        return 0x3F;
    case 0xF7:  // 16-bit rupee totals
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
        return 0xFF;
    default:
        return 0;
    }
}

// Without shareWoodenShield the Ordon and Wooden Shields stay as our save has them (burnt = gone).
void mergePlayer(dSv_player_c& local, dSv_player_c& remote, bool shareWoodenShield) {
    dSv_player_status_a_c& la = local.getPlayerStatusA();
    dSv_player_status_a_c& ra = remote.getPlayerStatusA();
    la.mMaxLife = maxOf<u16>(la.mMaxLife, ra.mMaxLife);
    la.mMaxOil = maxOf<u16>(la.mMaxOil, ra.mMaxOil);
    la.mWalletSize = maxOf<u8>(la.mWalletSize, ra.mWalletSize);
    la.mMaxMagic = maxOf<u8>(la.mMaxMagic, ra.mMaxMagic);
    la.mMagicFlag |= ra.mMagicFlag;
    for (int i = 0; i < MAX_EQUIPMENT; i++) {
        if (la.mSelectEquip[i] == dItemNo_NONE_e && ra.mSelectEquip[i] != dItemNo_NONE_e &&
            (shareWoodenShield || !isWoodenShield(ra.mSelectEquip[i])))
        {
            la.mSelectEquip[i] = ra.mSelectEquip[i];
        }
    }

    dSv_player_status_b_c& lb = local.getPlayerStatusB();
    dSv_player_status_b_c& rb = remote.getPlayerStatusB();
    lb.mTransformLevelFlag |= rb.mTransformLevelFlag;
    lb.mDarkClearLevelFlag |= rb.mDarkClearLevelFlag;

    dSv_player_item_c& li = local.getItem();
    dSv_player_item_c& ri = remote.getItem();
    for (int slot = 0; slot < MAX_ITEM_SLOTS; slot++) {
        // Ooccoo is situational and per player.
        if (slot == SLOT_18) {
            continue;
        }
        if (li.mItems[slot] != dItemNo_NONE_e || ri.mItems[slot] == dItemNo_NONE_e) {
            continue;
        }
        if (isBottleSlot(slot)) {
            li.mItems[slot] = dItemNo_EMPTY_BOTTLE_e;
        } else {
            li.mItems[slot] = ri.mItems[slot];
            if (isBombBagSlot(slot)) {
                const u8 bag = static_cast<u8>(slot - kBombBagSlotFirst);
                local.getItemRecord().mBombNum[bag] = remote.getItemRecord().mBombNum[bag];
            }
        }
    }
    // The Clawshot replaces the Hookshot (as card_to_memory fixes it up).
    if (li.mItems[SLOT_9] == dItemNo_HOOKSHOT_e && li.mItems[SLOT_10] == dItemNo_W_HOOKSHOT_e) {
        li.setItem(SLOT_9, dItemNo_NONE_e);
    }
    li.setLineUpItem();

    dSv_player_get_item_c& lg = local.getGetItem();
    dSv_player_get_item_c& rg = remote.getGetItem();
    const bool hadWood = lg.isFirstBit(dItemNo_WOOD_SHIELD_e);
    const bool hadOrdon = lg.isFirstBit(dItemNo_SHIELD_e);
    for (int i = 0; i < 8; i++) {
        lg.mItemFlags[i] = static_cast<u32>(lg.mItemFlags[i]) | static_cast<u32>(rg.mItemFlags[i]);
    }
    if (!shareWoodenShield) {
        if (!hadWood)
            lg.offFirstBit(dItemNo_WOOD_SHIELD_e);
        if (!hadOrdon)
            lg.offFirstBit(dItemNo_SHIELD_e);
    }

    dSv_player_item_max_c& lm = local.getItemMax();
    dSv_player_item_max_c& rm = remote.getItemMax();
    for (int i = 0; i < 8; i++) {
        lm.mItemMax[i] = maxOf(lm.mItemMax[i], rm.mItemMax[i]);
    }

    dSv_player_collect_c& lc = local.getCollect();
    dSv_player_collect_c& rc = remote.getCollect();
    for (int i = 0; i < 8; i++) {
        lc.mItem[i] |= rc.mItem[i];
    }
    lc.mCrystal |= rc.mCrystal;
    lc.mMirror |= rc.mMirror;
    lc.mPohNum = maxOf(lc.mPohNum, rc.mPohNum);

    dSv_light_drop_c& ld = local.getLightDrop();
    dSv_light_drop_c& rd = remote.getLightDrop();
    for (int i = 0; i < 4; i++) {
        ld.mLightDropNum[i] = maxOf(ld.mLightDropNum[i], rd.mLightDropNum[i]);
    }
    ld.mLightDropGetFlag |= rd.mLightDropGetFlag;

    dSv_letter_info_c& ll = local.getLetterInfo();
    dSv_letter_info_c& rl = remote.getLetterInfo();
    for (int i = 0; i < 2; i++) {
        ll.mLetterGetFlags[i] =
            static_cast<u32>(ll.mLetterGetFlags[i]) | static_cast<u32>(rl.mLetterGetFlags[i]);
    }
}

// What a merge made new to us, for the item toasts.
struct MergeBaseline {
    dSv_player_get_item_c getItem;
    u16 maxLife = 0;
    uint8_t dungeonItems[dSv_save_c::STAGE_MAX] = {};
};

MergeBaseline mergeBaseline(dSv_save_c& save) {
    MergeBaseline base;
    base.getItem = save.getPlayer().getGetItem();
    base.maxLife = save.getPlayer().getPlayerStatusA().getMaxLife();
    for (int s = 0; s < dSv_save_c::STAGE_MAX; s++) {
        base.dungeonItems[s] = save.mSave[s].getBit().mDungeonItem;
    }
    return base;
}

std::vector<item_toasts::ItemCount> mergedItems(const MergeBaseline& before, dSv_save_c& save) {
    std::vector<item_toasts::ItemCount> items;
    const dSv_player_get_item_c& after = save.getPlayer().getGetItem();
    for (int item = 0; item < 256; item++) {
        const u8 no = static_cast<u8>(item);
        if (no != dItemNo_KAKERA_HEART_e && no != dItemNo_UTAWA_HEART_e && isSyncedItemNo(no) &&
            after.isFirstBit(no) && !before.getItem.isFirstBit(no))
        {
            items.push_back({no, 1, -1});
        }
    }
    // 5 pieces make a heart.
    const int lifeGain = save.getPlayer().getPlayerStatusA().getMaxLife() - before.maxLife;
    if (lifeGain > 0) {
        if (lifeGain / 5 > 0) {
            items.push_back({dItemNo_UTAWA_HEART_e, lifeGain / 5, -1});
        }
        if (lifeGain % 5 > 0) {
            items.push_back({dItemNo_KAKERA_HEART_e, lifeGain % 5, -1});
        }
    }
    for (int s = 0; s < dSv_save_c::STAGE_MAX; s++) {
        const uint8_t gained = save.mSave[s].getBit().mDungeonItem & ~before.dungeonItems[s];
        if ((gained & (1 << dSv_memBit_c::MAP)) != 0) {
            items.push_back({dItemNo_MAP_e, 1, s});
        }
        if ((gained & (1 << dSv_memBit_c::COMPASS)) != 0) {
            items.push_back({dItemNo_COMPUS_e, 1, s});
        }
        if ((gained & (1 << dSv_memBit_c::BOSS_KEY)) != 0) {
            items.push_back({dItemNo_BOSS_KEY_e, 1, s});
        }
    }
    return items;
}

}  // namespace

void sendRequestWorldState() {
    State& st = state();
    const Session& session = Session::instance();
    if (!enabled() || !isSaveLoaded() || session.selfClientId() == 0) {
        return;
    }
    nlohmann::json packet = {{"type", "REQUEST_WORLD_STATE"}};
    // The server holds the team catch-up (cache + queue) back until we can apply it.
    if (!st.catchUpRequested) {
        packet["catchUp"] = true;
        st.catchUpRequested = true;
        item_toasts::beginCatchUp();
        // Taken now: replayed deltas touch these stages before the snapshot that holds them.
        st.keyCountAdoptable = untouchedStageMask(*dComIfGs_getSaveInfo(), currentSaveTblNo());
    }
    stampWorldPacket(packet, false);
    send(std::move(packet));
}

void sendUpdateWorldState(uint32_t targetClientId) {
    const Session& session = Session::instance();
    if (!enabled() || !isSaveLoaded() || session.selfClientId() == 0) {
        return;
    }
    dSv_info_c* info = dComIfGs_getSaveInfo();
    const int saveTbl = currentSaveTblNo();
    if (saveTbl >= 0) {
        // Fold the live stage flags into the save, as dStage_Delete does.
        info->putSave(saveTbl);
    }
    const dSv_save_c& save = info->getSavedata();
    const dSv_danBit_c& dan = info->getDan();
    nlohmann::json packet = {
        {"type", "UPDATE_WORLD_STATE"},
        {"fmt", kFormat},
        {"save", encodeBytes(&save, sizeof(dSv_save_c))},
        {"dan", encodeBytes(&dan, sizeof(dSv_danBit_c))},
        {"danStageNo", static_cast<int>(dan.mStageNo)},
        {"saveTblNo", saveTbl},
        {"quiet", true},
    };
    if (targetClientId != 0) {
        packet["targetClientId"] = targetClientId;
    }
    stampWorldPacket(packet, false);
    send(std::move(packet));
}

void handleRequestWorldState(const nlohmann::json& packet) {
    if (!acceptsWorldPacket(packet, "REQUEST_WORLD_STATE")) {
        return;
    }
    sendUpdateWorldState(packet.value("clientId", 0u));
}

void handleUpdateWorldState(const nlohmann::json& packet) {
    State& st = state();
    if (!acceptsWorldPacket(packet, "UPDATE_WORLD_STATE")) {
        return;
    }
    if (packet.value("fmt", std::string{}) != kFormat) {
        TwiliLog.warn(
            "[sync] UPDATE_WORLD_STATE in unknown format '{}'", packet.value("fmt", std::string{}));
        return;
    }
    dSv_save_c remote;
    if (!decodeBytes(packet, "save", &remote, sizeof(remote))) {
        TwiliLog.warn("[sync] UPDATE_WORLD_STATE has no decodable save blob");
        return;
    }

    const Session& session = Session::instance();
    dSv_info_c* info = dComIfGs_getSaveInfo();
    const int saveTbl = currentSaveTblNo();
    const dSv_memBit_c& liveBit = info->getMemory().getBit();
    const int liveKeyNumBefore = liveBit.mKeyNum;
    const uint8_t liveDungeonItemsBefore = liveBit.mDungeonItem;
    // A broadcast that beats our catch-up request: nothing was replayed yet.
    if (!st.catchUpRequested) {
        st.keyCountAdoptable = untouchedStageMask(*info, saveTbl);
    }
    if (saveTbl >= 0) {
        info->putSave(saveTbl);
    }

    RemoteApplyGuard guard;

    dSv_save_c& local = info->getSavedata();
    const MergeBaseline before = mergeBaseline(local);
    mergePlayer(local.getPlayer(), remote.getPlayer(), session.roomState().shareWoodenShield);
    for (int s = 0; s < dSv_save_c::STAGE_MAX; s++) {
        mergeMemBit(local.mSave[s].getBit(), remote.mSave[s].getBit(),
            ((st.keyCountAdoptable >> s) & 1u) != 0, st.cleared.switches[s]);
    }
    st.keyCountAdoptable = 0;
    for (int s = 0; s < dSv_save_c::STAGE2_MAX; s++) {
        for (int i = 0; i < 2; i++) {
            local.mSave2[s].mVisitedRoom[i] = static_cast<u32>(local.mSave2[s].mVisitedRoom[i]) |
                                              static_cast<u32>(remote.mSave2[s].mVisitedRoom[i]);
        }
    }
    for (int i = 0; i < MAX_EVENTS; i++) {
        const auto keepLocal = static_cast<uint8_t>(
            eventRegisterMask(i) | localOnlyEventMask(i) | st.cleared.eventBits[i]);
        local.mEvent.mEvent[i] |= static_cast<uint8_t>(remote.mEvent.mEvent[i] & ~keepLocal);
    }

    if (saveTbl >= 0) {
        info->getSave(saveTbl);
    }

    // Dungeon bits are not saved and only mean something inside that dungeon.
    dSv_danBit_c remoteDan;
    if (decodeBytes(packet, "dan", &remoteDan, sizeof(remoteDan)) && remoteDan.mStageNo >= 0 &&
        remoteDan.mStageNo == info->getDan().mStageNo)
    {
        dSv_danBit_c& dan = info->getDan();
        for (int i = 0; i < 2; i++) {
            dan.mSwitch[i] |= remoteDan.mSwitch[i];
        }
        for (int i = 0; i < 4; i++) {
            dan.mItem[i] |= remoteDan.mItem[i];
        }
    }

    shiftDungeonItemBaseline(liveKeyNumBefore, liveDungeonItemsBefore);

    // The server's cache and an answer to our request are the catch-up; a broadcast is live.
    const uint32_t from = packet.value("clientId", 0u);
    const bool catchUp = packet.value("fromCache", false) ||
                         (session.selfClientId() != 0 &&
                             packet.value("targetClientId", 0u) == session.selfClientId());
    item_toasts::noteMerge(item_toasts::senderOf(session, packet),
        catchUp ? item_toasts::Source::CatchUpMerge : item_toasts::Source::LiveMerge,
        mergedItems(before, local));

    st.merges[from]++;
    st.stats.merges++;
    TwiliLog.info("[sync] merged world state from client {}{} (their stage table {}, ours {})",
        from, packet.value("fromCache", false) ? " (server cache)" : "",
        packet.value("saveTblNo", -1), saveTbl);
}

}  // namespace detail
}  // namespace twili::sync
