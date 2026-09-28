// GIVE_ITEM: pickups replayed on teammates through ItemService (other mods' item logic applies).

#include "sync/WorldSyncState.hpp"

#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/ItemToasts.hpp"

#include "d/d_item.h"
#include "d/d_item_data.h"

#include <mods/svc/item.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace twili::sync {
namespace detail {
namespace {

// Check name on our own grants: their give notification is our echo.
constexpr const char* kRemoteGrant = "twili_together:remote";

ItemGiveHandle s_observer = 0;

bool isRepeatableItem(uint8_t itemNo) {
    switch (itemNo) {
    case dItemNo_KAKERA_HEART_e:
    case dItemNo_UTAWA_HEART_e:
    case dItemNo_EMPTY_BOTTLE_e:
    case dItemNo_BOMB_BAG_LV1_e:
    case dItemNo_BOMB_IN_BAG_e:
        return true;
    default:
        return false;
    }
}

// `line` lists the items that take turns in one slot, in game order; a grant may only move forward.
bool slotRefusesGrant(int slot, std::initializer_list<uint8_t> line, uint8_t itemNo) {
    const uint8_t held = dComIfGs_getItem(slot, false);
    if (held == dItemNo_NONE_e) {
        return false;
    }
    for (const uint8_t step : line) {
        if (step == itemNo) {
            return true;
        }
        if (step == held) {
            return false;
        }
    }
    return true;
}

bool receiverHasItem(uint8_t itemNo) {
    // These set a capacity outright; the first-get bit says nothing about the level reached.
    switch (itemNo) {
    case dItemNo_WALLET_LV1_e:
        return dComIfGs_getWalletSize() >= WALLET;
    case dItemNo_WALLET_LV2_e:
        return dComIfGs_getWalletSize() >= BIG_WALLET;
    case dItemNo_WALLET_LV3_e:
        return dComIfGs_getWalletSize() >= GIANT_WALLET;
    case dItemNo_ARROW_LV1_e:
    case dItemNo_ARROW_LV2_e:
        return dComIfGs_getArrowMax() >= BIG_QUIVER_MAX;
    case dItemNo_ARROW_LV3_e:
        return dComIfGs_getArrowMax() >= GIANT_QUIVER_MAX;
    default:
        break;
    }

    // checkItemGet also sees what was set directly (the Master Sword from its pedestal).
    if (dComIfGs_isItemFirstBit(itemNo) || checkItemGet(itemNo, FALSE) > 0) {
        return true;
    }

    switch (itemNo) {
    case dItemNo_BOW_e:
    case dItemNo_LIGHT_ARROW_e:
        return slotRefusesGrant(SLOT_4, {dItemNo_BOW_e, dItemNo_LIGHT_ARROW_e}, itemNo);
    case dItemNo_HOOKSHOT_e:
        // A Clawshot would sit next to the pair.
        return dComIfGs_getItem(SLOT_10, false) != dItemNo_NONE_e;
    case dItemNo_RAFRELS_MEMO_e:
    case dItemNo_ASHS_SCRIBBLING_e:
        return slotRefusesGrant(
            SLOT_19, {dItemNo_RAFRELS_MEMO_e, dItemNo_ASHS_SCRIBBLING_e}, itemNo);
    case dItemNo_LETTER_e:
    case dItemNo_BILL_e:
    case dItemNo_WOOD_STATUE_e:
    case dItemNo_IRIAS_PENDANT_e:
    case dItemNo_HORSE_FLUTE_e:
        return slotRefusesGrant(SLOT_21,
            {dItemNo_LETTER_e, dItemNo_BILL_e, dItemNo_WOOD_STATUE_e, dItemNo_IRIAS_PENDANT_e,
                dItemNo_HORSE_FLUTE_e},
            itemNo);
    case dItemNo_ANCIENT_DOCUMENT_e:
    case dItemNo_AIR_LETTER_e:
    case dItemNo_ANCIENT_DOCUMENT2_e:
        return slotRefusesGrant(SLOT_22,
            {dItemNo_ANCIENT_DOCUMENT_e, dItemNo_AIR_LETTER_e, dItemNo_ANCIENT_DOCUMENT2_e},
            itemNo);
    default:
        return false;
    }
}

// Worn gear is kept even for an upgrade (Link reloads models only when asked); empty entries fill.
struct KeptProgress {
    uint8_t walletSize;
    uint8_t arrowMax;
    uint8_t equip[MAX_EQUIPMENT];
};

KeptProgress captureProgress() {
    const dSv_player_status_a_c& status = dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusA();
    KeptProgress kept;
    kept.walletSize = status.getWalletSize();
    kept.arrowMax = dComIfGs_getArrowMax();
    for (int i = 0; i < MAX_EQUIPMENT; i++) {
        kept.equip[i] = status.getSelectEquip(i);
    }
    return kept;
}

void restoreProgress(const KeptProgress& kept) {
    dSv_player_status_a_c& status = dComIfGs_getSaveInfo()->getPlayer().getPlayerStatusA();
    if (status.getWalletSize() < kept.walletSize) {
        status.setWalletSize(kept.walletSize);
    }
    if (dComIfGs_getArrowMax() < kept.arrowMax) {
        dComIfGs_setArrowMax(kept.arrowMax);
    }
    for (int i = 0; i < MAX_EQUIPMENT; i++) {
        if (kept.equip[i] == dItemNo_NONE_e) {
            continue;
        }
        // The Light Sword is the Master Sword infused; the plain one is no longer offered.
        if (i == COLLECT_SWORD && kept.equip[i] == dItemNo_MASTER_SWORD_e &&
            status.getSelectEquip(i) == dItemNo_LIGHT_SWORD_e)
        {
            continue;
        }
        status.setSelectEquip(i, kept.equip[i]);
    }
}

// The grant ItemService is dispatching right now, between execItemGet's pre and post hooks.
struct ActiveGrant {
    bool active = false;
    int depth = 0;
    uint8_t item = 0;
    KeptProgress kept{};
    bool switch28 = false;
    bool stageLife = false;
    bool heldEarring = false;
};

ActiveGrant s_grant;
int s_itemGetDepth = 0;

constexpr int kWoodStickSwitch = 28;

bool liveSwitch28() {
    const dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    return (static_cast<u32>(bit.mSwitch[kWoodStickSwitch >> 5]) >> (kWoodStickSwitch & 31)) & 1u;
}

bool liveStageLife() {
    return (dComIfGs_getSaveInfo()->getMemory().getBit().mDungeonItem &
               (1 << dSv_memBit_c::STAGE_LIFE)) != 0;
}

// Undoes what an item function does to the stage the receiver happens to be in.
void undoStageEffects(const ActiveGrant& grant) {
    dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
    if (grant.item == dItemNo_WOOD_STICK_e && !grant.switch28 && liveSwitch28()) {
        bit.mSwitch[kWoodStickSwitch >> 5] =
            static_cast<u32>(bit.mSwitch[kWoodStickSwitch >> 5]) & ~(1u << (kWoodStickSwitch & 31));
    }
    if (grant.item == dItemNo_UTAWA_HEART_e && !grant.stageLife && liveStageLife()) {
        bit.mDungeonItem &= static_cast<u8>(~(1 << dSv_memBit_c::STAGE_LIFE));
    }
    // A coral earring that got here first folds into the jewel rod, as ZORAS_JEWEL does.
    if (grant.item == dItemNo_FISHING_ROD_1_e && grant.heldEarring) {
        dComIfGs_setRodTypeLevelUp();
    }
}

void onItemGiven(ModContext*, const ItemGiveInfo* info, void*) {
    if (info == nullptr) {
        return;
    }
    State& st = state();
    if (info->check_name != nullptr && std::strcmp(info->check_name, kRemoteGrant) == 0) {
        const auto it = std::find(st.pendingGrants.begin(), st.pendingGrants.end(), info->item);
        if (it != st.pendingGrants.end()) {
            st.pendingGrants.erase(it);
        }
        st.stats.grantsApplied++;
        TwiliLog.info("[sync] applied item 0x{:02X} from a teammate", info->item);
        return;
    }
    if (RemoteApplyGuard::active()) {
        return;
    }
    sendGiveItem(info->item);
}

}  // namespace

void sendGiveItem(uint8_t itemNo) {
    if (!Session::active() || !isSaveLoaded()) {
        return;
    }
    const Session& session = Session::instance();
    // Keys, maps and compasses come through here too, and pickups with sync off.
    if (session.isConnected()) {
        item_toasts::noteOwnItem(itemNo, currentSaveTblNo());
    }
    if (!enabled() || !isSyncedItemNo(itemNo) ||
        (isWoodenShield(itemNo) && !session.roomState().shareWoodenShield))
    {
        return;
    }
    nlohmann::json packet = {
        {"type", "GIVE_ITEM"},
        {"itemNo", static_cast<unsigned>(itemNo)},
    };
    stampWorldPacket(packet, true);
    send(std::move(packet));
}

void handleGiveItem(const nlohmann::json& packet) {
    State& st = state();
    if (!acceptsWorldPacket(packet, "GIVE_ITEM")) {
        return;
    }
    const Session& session = Session::instance();
    const int itemNo = packet.value("itemNo", -1);
    // A shield grant queued before the room made wooden shields personal is dropped too.
    if (!isSyncedItemNo(itemNo) ||
        (isWoodenShield(itemNo) && !session.roomState().shareWoodenShield))
    {
        return;
    }
    const uint8_t item = static_cast<uint8_t>(itemNo);
    const uint32_t senderId = packet.value("clientId", 0u);
    const auto source =
        packet.value("fromQueue", false) ? item_toasts::Source::Replay : item_toasts::Source::Live;
    const bool pending =
        std::find(st.pendingGrants.begin(), st.pendingGrants.end(), item) != st.pendingGrants.end();
    if (!isRepeatableItem(item) && (pending || receiverHasItem(item))) {
        TwiliLog.info(
            "[sync] skipped item 0x{:02X} from client {}: already have it", itemNo, senderId);
        item_toasts::noteItem(item_toasts::senderOf(session, packet), item, 1, -1, source, false);
        return;
    }
    if (svc_item == nullptr ||
        svc_item->give_item(mod_ctx, kRemoteGrant, item, ITEM_GIVE_SILENT) != MOD_OK)
    {
        TwiliLog.warn("[sync] cannot grant item 0x{:02X}: ItemService unavailable", itemNo);
        return;
    }
    st.pendingGrants.push_back(item);
    st.stats.grantsQueued++;
    TwiliLog.info("[sync] received item 0x{:02X} from client {}", itemNo, senderId);
    item_toasts::noteItem(item_toasts::senderOf(session, packet), item, 1, -1, source, true);
}

}  // namespace detail

using namespace detail;

bool isSyncedItemNo(int itemNo) {
    if (itemNo < 0 || itemNo > 0xFF) {
        return false;
    }
    // Hearts, rupees, magic, ammo, fairies.
    if (itemNo <= dItemNo_TRIPLE_HEART_e) {
        return false;
    }
    // Bottle contents and refills (the empty bottle itself is progression).
    if (itemNo > dItemNo_EMPTY_BOTTLE_e && itemNo <= dItemNo_LV3_SOUP_e) {
        return false;
    }
    if (itemNo >= dItemNo_CHUCHU_YELLOW2_e && itemNo <= dItemNo_LIGHT_DROP_e) {
        return false;
    }
    switch (itemNo) {
    // Dungeon items travel as UPDATE_DUNGEON_ITEMS.
    case dItemNo_SMALL_KEY_e:
    case dItemNo_SMALL_KEY2_e:
    case dItemNo_KEY_OF_FILONE_e:
    case dItemNo_BOSS_KEY_e:
    case dItemNo_LV2_BOSS_KEY_e:
    case dItemNo_LV5_BOSS_KEY_e:
    case dItemNo_MAP_e:
    case dItemNo_COMPUS_e:
    // Ooccoo and the note left in her slot: situational, per player.
    case dItemNo_DUNGEON_EXIT_e:
    case dItemNo_DUNGEON_EXIT_2_e:
    case dItemNo_DUNGEON_BACK_e:
    case dItemNo_LV7_DUNGEON_EXIT_e:
    case dItemNo_TKS_LETTER_e:
    // A rupee grant disguised as an item.
    case dItemNo_LINKS_SAVINGS_e:
    case dItemNo_NONE_e:
        return false;
    default:
        return true;
    }
}

bool isWoodenShield(int itemNo) {
    return itemNo == dItemNo_WOOD_SHIELD_e || itemNo == dItemNo_SHIELD_e;
}

bool installItemObserver() {
    if (svc_item == nullptr) {
        TwiliLog.warn("[sync] ItemService unavailable: items will not be shared");
        return false;
    }
    if (s_observer == 0 &&
        svc_item->observe_gives(mod_ctx, onItemGiven, nullptr, &s_observer) != MOD_OK)
    {
        TwiliLog.warn("[sync] cannot observe item grants: items will not be shared");
        s_observer = 0;
        return false;
    }
    return true;
}

void removeItemObserver() {
    if (svc_item != nullptr && s_observer != 0) {
        svc_item->unobserve_gives(mod_ctx, s_observer);
    }
    s_observer = 0;
    s_grant = {};
    s_itemGetDepth = 0;
}

void beforeItemGet(uint8_t itemNo) {
    State& st = state();
    s_itemGetDepth++;
    // ItemService dispatches after mod_update; the window ends at the frame's actor pass.
    if (s_grant.active || !st.grantWindow || st.pendingGrants.empty() ||
        st.pendingGrants.front() != itemNo)
    {
        return;
    }
    s_grant.active = true;
    s_grant.depth = s_itemGetDepth;
    s_grant.item = itemNo;
    s_grant.kept = captureProgress();
    s_grant.switch28 = liveSwitch28();
    s_grant.stageLife = liveStageLife();
    s_grant.heldEarring = dComIfGs_getItem(SLOT_20, false) == dItemNo_ZORAS_JEWEL_e;
    RemoteApplyGuard::enter();
}

void afterItemGet(uint8_t itemNo) {
    if (s_grant.active && s_grant.depth == s_itemGetDepth && s_grant.item == itemNo) {
        undoStageEffects(s_grant);
        restoreProgress(s_grant.kept);
        s_grant.active = false;
        RemoteApplyGuard::leave();
    }
    if (s_itemGetDepth > 0) {
        s_itemGetDepth--;
    }
}

void closeGrantWindow() {
    state().grantWindow = false;
}

}  // namespace twili::sync
