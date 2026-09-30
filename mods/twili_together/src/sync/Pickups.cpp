// SHARE_PICKUP: a teammate's consumable pickups are added here too, within our own limits, and a
// randomizer trap one of them sprang goes off here too. Live only: never queued or replayed.

#include "sync/WorldSyncState.hpp"

#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/ItemToasts.hpp"
#include "ui/Toasts.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_item_data.h"

#include <fmt/format.h>
#include <mods/items.h>
#include <mods/svc/item.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace twili::sync {
namespace detail {
namespace {

// Check name on our trap grants: their give notification is our echo.
constexpr const char* kTrapGrant = "twili_together:trap";
constexpr size_t kMaxItemsPerPacket = 64;

enum class Gain : uint8_t { None, Rupees, Life, Arrows, Seeds, Bombs };

struct GainOf {
    Gain kind = Gain::None;
    int amount = 0;
    uint8_t bombKind = 0;
};

// What the game's item function adds (d_item.cpp); items whose function adds nothing are left out.
GainOf gainOf(uint8_t itemNo) {
    switch (itemNo) {
    case dItemNo_HEART_e: return {Gain::Life, 4};
    case dItemNo_RECOVERY_FAILY_e: return {Gain::Life, 32};
    case dItemNo_GREEN_RUPEE_e: return {Gain::Rupees, 1};
    case dItemNo_BLUE_RUPEE_e: return {Gain::Rupees, 5};
    case dItemNo_YELLOW_RUPEE_e: return {Gain::Rupees, 10};
    case dItemNo_RED_RUPEE_e: return {Gain::Rupees, 20};
    case dItemNo_PURPLE_RUPEE_e: return {Gain::Rupees, 50};
    case dItemNo_ORANGE_RUPEE_e: return {Gain::Rupees, 100};
    case dItemNo_SILVER_RUPEE_e: return {Gain::Rupees, 200};
    case dItemNo_LINKS_SAVINGS_e: return {Gain::Rupees, 50};
    case dItemNo_ARROW_10_e: return {Gain::Arrows, 10};
    case dItemNo_ARROW_20_e: return {Gain::Arrows, 20};
    case dItemNo_ARROW_30_e: return {Gain::Arrows, 30};
    case dItemNo_ARROW_1_e: return {Gain::Arrows, 1};
    case dItemNo_PACHINKO_SHOT_e: return {Gain::Seeds, 50};
    case dItemNo_BOMB_5_e: return {Gain::Bombs, 5, dItemNo_NORMAL_BOMB_e};
    case dItemNo_BOMB_10_e: return {Gain::Bombs, 10, dItemNo_NORMAL_BOMB_e};
    case dItemNo_BOMB_20_e: return {Gain::Bombs, 20, dItemNo_NORMAL_BOMB_e};
    case dItemNo_BOMB_30_e: return {Gain::Bombs, 30, dItemNo_NORMAL_BOMB_e};
    case dItemNo_WATER_BOMB_5_e: return {Gain::Bombs, 5, dItemNo_WATER_BOMB_e};
    case dItemNo_WATER_BOMB_10_e: return {Gain::Bombs, 10, dItemNo_WATER_BOMB_e};
    case dItemNo_WATER_BOMB_20_e: return {Gain::Bombs, 15, dItemNo_WATER_BOMB_e};
    case dItemNo_WATER_BOMB_30_e: return {Gain::Bombs, 3, dItemNo_WATER_BOMB_e};
    case dItemNo_BOMB_INSECT_5_e: return {Gain::Bombs, 5, dItemNo_POKE_BOMB_e};
    case dItemNo_BOMB_INSECT_10_e: return {Gain::Bombs, 10, dItemNo_POKE_BOMB_e};
    case dItemNo_BOMB_INSECT_20_e: return {Gain::Bombs, 3, dItemNo_POKE_BOMB_e};
    default: return {};
    }
}

std::vector<uint8_t> s_outgoing;
int s_bottleFairyDepth = 0;

bool startsWith(const char* s, std::string_view prefix) {
    return s != nullptr && std::strncmp(s, prefix.data(), prefix.size()) == 0;
}

bool ownsBow() {
    const uint8_t held = dComIfGs_getItem(SLOT_4, false);
    return held == dItemNo_BOW_e || held == dItemNo_LIGHT_ARROW_e;
}

bool ownsSlingshot() {
    return dComIfGs_getItem(SLOT_23, true) == dItemNo_PACHINKO_e;
}

// Not while knocked out, in a game over or without a live Link to take it.
bool canHeal() {
    daAlink_c* link = local::liveLink();
    return link != nullptr && dComIfGs_getLife() > 0 && link->mProcID != daAlink_c::PROC_DEAD;
}

// The same count the game's item function adds; the meter caps it at the wallet, quiver, bag size.
bool applyGain(const GainOf& gain) {
    State& st = state();
    switch (gain.kind) {
    case Gain::Rupees:
        dComIfGp_setItemRupeeCount(gain.amount);
        st.stats.rupeesAdded += gain.amount;
        return true;
    case Gain::Life:
        if (!canHeal()) {
            return false;
        }
        dComIfGp_setItemLifeCount(static_cast<f32>(gain.amount), 0);
        st.stats.lifeAdded += gain.amount;
        return true;
    case Gain::Arrows:
        if (!ownsBow()) {
            return false;
        }
        dComIfGp_setItemArrowNumCount(static_cast<s16>(gain.amount));
        st.stats.arrowsAdded += gain.amount;
        return true;
    case Gain::Seeds:
        if (!ownsSlingshot()) {
            return false;
        }
        dComIfGp_setItemPachinkoNumCount(static_cast<s16>(gain.amount));
        st.stats.seedsAdded += gain.amount;
        return true;
    case Gain::Bombs: {
        // Fills bags of that kind and empty bags, like the game's own pickup; returns what did not fit.
        const int left = addBombCount(gain.bombKind, static_cast<u8>(gain.amount));
        st.stats.bombsAdded += gain.amount - left;
        return left < gain.amount;
    }
    case Gain::None:
        break;
    }
    return false;
}

void grantTrap(const nlohmann::json& packet) {
    State& st = state();
    st.stats.trapsReceived++;
    if (svc_item == nullptr ||
        svc_item->give_item(mod_ctx, kTrapGrant, kFoolishItem, ITEM_GIVE_SILENT) != MOD_OK)
    {
        TwiliLog.warn("[sync] cannot pass on a teammate's trap: ItemService unavailable");
        return;
    }
    const item_toasts::Sender who = item_toasts::senderOf(Session::instance(), packet);
    TwiliLog.info("[sync] trap from client {} queued", who.clientId);
    ui::toastInline("Trap",
        fmt::format("<b>{}</b> sprang a trap. It gets you too!",
            ui::escapeRml(who.name.empty() ? std::string("A teammate") : who.name)),
        ui::kToastWarning, 3000);
}

}  // namespace

void notePickup(uint8_t itemNo, const char* checkName) {
    if (!enabled() || !isSaveLoaded() || RemoteApplyGuard::active()) {
        return;
    }
    if (itemNo == kFoolishItem) {
        if (randoGame()) {
            s_outgoing.push_back(itemNo);
            state().stats.trapsSent++;
        }
        return;
    }
    // Purchases, a bottled fairy and a tear's heal are no pickups.
    if (gainOf(itemNo).kind == Gain::None || !Session::instance().roomState().sharePickups ||
        startsWith(checkName, ITEM_CHECK_SHOP_PREFIX) || s_bottleFairyDepth > 0 || insideDropGet())
    {
        return;
    }
    s_outgoing.push_back(itemNo);
    state().stats.pickupsSent++;
}

void flushPickups() {
    if (s_outgoing.empty()) {
        return;
    }
    if (!enabled()) {
        s_outgoing.clear();
        return;
    }
    while (!s_outgoing.empty()) {
        const size_t n = std::min(s_outgoing.size(), kMaxItemsPerPacket);
        nlohmann::json packet = {
            {"type", "SHARE_PICKUP"},
            {"items", std::vector<uint8_t>(s_outgoing.begin(), s_outgoing.begin() + n)},
        };
        stampWorldPacket(packet, false);
        // Droppable: a backlog flushed after a reconnect would hand out stale pickups.
        Session::instance().send(packet, net::Delivery::Droppable);
        s_outgoing.erase(s_outgoing.begin(), s_outgoing.begin() + n);
    }
}

void handleSharePickup(const nlohmann::json& packet) {
    if (packet.value("fromQueue", false) || packet.value("fromCache", false) ||
        !acceptsWorldPacket(packet, "SHARE_PICKUP"))
    {
        return;
    }
    const auto items = packet.find("items");
    if (items == packet.end() || !items->is_array()) {
        return;
    }
    State& st = state();
    const bool share = Session::instance().roomState().sharePickups;
    const bool rando = randoGame();
    RemoteApplyGuard guard;
    size_t seen = 0;
    for (const auto& entry : *items) {
        if (++seen > kMaxItemsPerPacket) {
            break;
        }
        if (!entry.is_number_unsigned() || entry.get<unsigned>() > 0xFF) {
            continue;
        }
        const auto itemNo = static_cast<uint8_t>(entry.get<unsigned>());
        if (itemNo == kFoolishItem) {
            if (rando) {
                grantTrap(packet);
            }
            continue;
        }
        const GainOf gain = gainOf(itemNo);
        if (!share || gain.kind == Gain::None) {
            continue;
        }
        if (applyGain(gain)) {
            st.stats.pickupsApplied++;
        } else {
            st.stats.pickupsSkipped++;
        }
    }
}

bool isTrapGrant(const char* checkName) {
    return checkName != nullptr && std::strcmp(checkName, kTrapGrant) == 0;
}

}  // namespace detail

void enterBottleFairy() {
    detail::s_bottleFairyDepth++;
}

void leaveBottleFairy() {
    if (detail::s_bottleFairyDepth > 0) {
        detail::s_bottleFairyDepth--;
    }
}

}  // namespace twili::sync
