#include "ui/ItemToasts.hpp"

#include "core/Config.hpp"
#include "core/LocalPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "ui/ColorMath.hpp"
#include "ui/ItemNames.hpp"
#include "ui/Toasts.hpp"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JUtility/JUTTexture.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_meter2_info.h"
#include "d/d_stage.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>

namespace twili::item_toasts {
namespace {

using clock = std::chrono::steady_clock;

constexpr auto kItemDuration = std::chrono::milliseconds(4000);
// While two or more wait behind it, so a backlog clears without hiding other toasts for long.
constexpr auto kBusyItemDuration = std::chrono::milliseconds(2500);
constexpr auto kSummaryDuration = std::chrono::milliseconds(5000);
// The overlay shows one toast at a time: ours is gone after its duration plus the fade.
constexpr auto kToastFade = std::chrono::milliseconds(400);
// The catch-up window closes this long after the last replay, within kWindowMin..kWindowMax.
constexpr auto kWindowQuiet = std::chrono::milliseconds(2500);
constexpr auto kWindowMin = std::chrono::milliseconds(2000);
constexpr auto kWindowMax = std::chrono::seconds(15);
// A window's items from one sender toast one by one up to this many, else as one summary.
constexpr int kIndividualMax = 2;
constexpr size_t kMaxPending = 5;
// Toasts held longer than this (a long cutscene) merge per sender when released.
constexpr auto kStaleAfter = std::chrono::seconds(60);
constexpr int kMaxSummaryIcons = 4;
constexpr size_t kHistoryMax = 64;
constexpr double kMinNameContrast = 3.0;

enum class Kind { Item, Own, Summary, Merged };

struct Entry {
    uint32_t seq = 0;
    Kind kind = Kind::Item;
    std::vector<Sender> senders;  // one, except for a summary
    std::vector<ItemCount> items;  // in arrival order
    int total = 0;
    clock::time_point queuedAt{};
};

struct Tally {
    Sender who;
    int total = 0;
    std::vector<ItemCount> items;
};

struct CatchUpWindow {
    bool open = false;
    clock::time_point openedAt{}, lastActivity{};
    std::vector<Tally> tallies;       // per sender, in order of first arrival
    std::vector<ItemCount> arrivals;  // every counted item, all senders, in order
};

struct IconInfo {
    bool known = false;
    IconProbe probe;
};

std::deque<Entry> sPending;
std::deque<Record> sHistory;
CatchUpWindow sWindow;
uint32_t sNextSeq = 1;
clock::time_point sOverlayBusyUntil{};
std::array<IconInfo, 256> sIcons;

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Item: return "item";
    case Kind::Own: return "own";
    case Kind::Summary: return "summary";
    case Kind::Merged: return "merged";
    }
    return "?";
}

bool sameSender(const Sender& a, const Sender& b) {
    return a.self == b.self && (a.self || a.clientId == b.clientId);
}

bool isDungeonItem(uint8_t itemNo) {
    switch (itemNo) {
    case dItemNo_SMALL_KEY_e:
    case dItemNo_MAP_e:
    case dItemNo_COMPUS_e:
    case dItemNo_BOSS_KEY_e:
    case dItemNo_LV2_BOSS_KEY_e:
    case dItemNo_LV5_BOSS_KEY_e:
        return true;
    default:
        return false;
    }
}

// Progression items: no consumables, bottle contents, dungeon-only or situational items.
bool isProgressionItem(uint8_t itemNo) {
    if (itemNo <= dItemNo_TRIPLE_HEART_e) {
        return false;
    }
    if (itemNo > dItemNo_EMPTY_BOTTLE_e && itemNo <= dItemNo_LV3_SOUP_e) {
        return false;
    }
    if (itemNo >= dItemNo_CHUCHU_YELLOW2_e && itemNo <= dItemNo_LIGHT_DROP_e) {
        return false;
    }
    switch (itemNo) {
    case dItemNo_SMALL_KEY_e:
    case dItemNo_SMALL_KEY2_e:
    case dItemNo_KEY_OF_FILONE_e:
    case dItemNo_BOSS_KEY_e:
    case dItemNo_LV2_BOSS_KEY_e:
    case dItemNo_LV5_BOSS_KEY_e:
    case dItemNo_MAP_e:
    case dItemNo_COMPUS_e:
    case dItemNo_DUNGEON_EXIT_e:
    case dItemNo_DUNGEON_EXIT_2_e:
    case dItemNo_DUNGEON_BACK_e:
    case dItemNo_LV7_DUNGEON_EXIT_e:
    case dItemNo_TKS_LETTER_e:
    case dItemNo_LINKS_SAVINGS_e:
    case dItemNo_NONE_e:
        return false;
    default:
        return true;
    }
}

// The item-get window's icon, read like the host's item:// provider; checksum of all layers.
const IconProbe& iconInfo(uint8_t itemNo) {
    IconInfo& info = sIcons[itemNo];
    if (info.known || dComIfGp_getItemIconArchive() == nullptr) {
        return info.probe;
    }
    struct alignas(32) Buffer {
        uint8_t bytes[0xC00];
    };
    std::array<Buffer, 4> buffers{};
    std::array<J2DPicture, 4> pictures{};
    const int layers = dMeter2Info_readItemTexture(itemNo, buffers[0].bytes, &pictures[0],
        buffers[1].bytes, &pictures[1], buffers[2].bytes, &pictures[2], buffers[3].bytes,
        &pictures[3], -1);
    info.known = true;
    IconProbe& p = info.probe;
    p.ok = layers > 0;
    p.layers = std::max(layers, 0);
    uint32_t crc = 2166136261u;
    for (int i = 0; i < std::min(layers, 4); ++i) {
        for (const uint8_t b : buffers[i].bytes) {
            crc = (crc ^ b) * 16777619u;
        }
    }
    p.checksum = p.ok ? crc : 0;
    if (p.ok) {
        const auto* timg = reinterpret_cast<const ResTIMG*>(buffers[0].bytes);
        p.width = timg->width;
        p.height = timg->height;
    }
    return p;
}

// The boss key the game shows for a dungeon's BOSS_KEY bit.
ItemCount displayed(ItemCount ic) {
    if (ic.itemNo == dItemNo_BOSS_KEY_e) {
        if (ic.saveTbl == dStage_SaveTbl_LV2) {
            ic.itemNo = dItemNo_LV2_BOSS_KEY_e;
        } else if (ic.saveTbl == dStage_SaveTbl_LV5) {
            ic.itemNo = dItemNo_LV5_BOSS_KEY_e;
        }
    }
    return ic;
}

// "Small Key ×2 (Goron Mines)": the count when above one, the dungeon when it is not ours.
std::string itemLabel(const ItemCount& ic) {
    std::string label = ui::itemDisplayName(ic.itemNo, ic.saveTbl);
    if (ic.count > 1) {
        label += fmt::format(" ×{}", ic.count);
    }
    if (isDungeonItem(ic.itemNo) && ic.saveTbl >= 0 &&
        ic.saveTbl != Session::instance().currentSaveTblNo())
    {
        const char* dungeon = ui::dungeonNameForSaveTbl(ic.saveTbl);
        if (dungeon[0] != '\0') {
            label += fmt::format(" ({})", dungeon);
        }
    }
    return label;
}

std::string senderText(const Sender& s, bool sentenceStart) {
    if (s.self) {
        return sentenceStart ? "You" : "you";
    }
    if (s.name.empty()) {
        return sentenceStart ? "A teammate" : "a teammate";
    }
    return s.name;
}

std::string senderRml(const Sender& s, bool sentenceStart) {
    const std::string text = ui::escapeRml(senderText(s, sentenceStart));
    if (!s.self && (s.name.empty() || !s.hasColor)) {
        return s.name.empty() ? text : fmt::format("<span class=\"player\">{}</span>", text);
    }
    const auto c = ui::color::readableOnTagFill({s.r, s.g, s.b}, kMinNameContrast);
    return fmt::format("<span class=\"player\" style=\"color: #{:02X}{:02X}{:02X};\">{}</span>",
        c.r, c.g, c.b, text);
}

// "Dad", "Dad and Mom", "3 teammates".
std::string sendersText(const std::vector<Sender>& senders, bool rml) {
    const auto one = [rml](const Sender& s) {
        return rml ? senderRml(s, false) : senderText(s, false);
    };
    if (senders.size() == 1) {
        return one(senders[0]);
    }
    if (senders.size() == 2) {
        return one(senders[0]) + " and " + one(senders[1]);
    }
    return fmt::format("{} teammates", senders.size());
}

std::string entryText(const Entry& e, bool rml) {
    const Sender& who = e.senders.front();
    const std::string items = e.total == 1 ? "1 item" : fmt::format("{} items", e.total);
    switch (e.kind) {
    case Kind::Item:
    case Kind::Own: {
        const std::string label = itemLabel(e.items.front());
        return rml ? fmt::format("{} got <b>{}</b>", senderRml(who, true), ui::escapeRml(label))
                   : fmt::format("{} got {}", senderText(who, true), label);
    }
    case Kind::Merged:
        return fmt::format("{} got {}", rml ? senderRml(who, true) : senderText(who, true), items);
    case Kind::Summary:
        return fmt::format("Synced {} from {}", items, sendersText(e.senders, rml));
    }
    return {};
}

std::string iconRml(uint8_t itemNo) {
    if (iconInfo(itemNo).ok) {
        return fmt::format("<img class=\"item-icon\" src=\"item://item/{:02x}\"/>", itemNo);
    }
    return "<icon class=\"item-fallback\"/>";
}

// The most recent distinct items, newest first.
std::vector<uint8_t> recentItems(const std::vector<ItemCount>& items, int max) {
    std::vector<uint8_t> out;
    for (auto it = items.rbegin(); it != items.rend() && static_cast<int>(out.size()) < max; ++it) {
        if (std::find(out.begin(), out.end(), it->itemNo) == out.end()) {
            out.push_back(it->itemNo);
        }
    }
    return out;
}

std::string entryRml(const Entry& e) {
    if (e.kind == Kind::Item || e.kind == Kind::Own) {
        return fmt::format("<row class=\"item-line\">{}<span class=\"text\">{}</span></row>",
            iconRml(e.items.front().itemNo), entryText(e, true));
    }
    std::string icons;
    const std::vector<uint8_t> shown = recentItems(e.items, kMaxSummaryIcons);
    for (const uint8_t itemNo : shown) {
        icons += iconRml(itemNo);
    }
    const int more = e.total - static_cast<int>(shown.size());
    if (more > 0) {
        icons += fmt::format("<span class=\"more\">+{}</span>", more);
    }
    // Text straight inside a flex row does not show in RmlUi: it goes in a span.
    return fmt::format("<row class=\"icons\">{}</row><row><span class=\"text\">{}</span></row>",
        icons, entryText(e, true));
}

Record* findRecord(uint32_t seq) {
    for (Record& r : sHistory) {
        if (r.seq == seq) {
            return &r;
        }
    }
    return nullptr;
}

void fillRecord(Record& r, const Entry& e) {
    r.seq = e.seq;
    r.kind = kindName(e.kind);
    r.senderId = e.senders.front().clientId;
    r.senderNames.clear();
    r.nameFromPacket = false;
    for (const Sender& s : e.senders) {
        r.senderNames.push_back(s.self ? "self" : s.name);
        r.nameFromPacket = r.nameFromPacket || s.nameFromPacket;
    }
    r.items.clear();
    for (const ItemCount& ic : e.items) {
        r.items.push_back(ic.itemNo);
    }
    r.count = e.kind == Kind::Item || e.kind == Kind::Own ? e.items.front().count : e.total;
    r.text = entryText(e, false);
    r.rml = entryRml(e);
    const IconProbe& icon = iconInfo(e.items.front().itemNo);
    r.iconOk = icon.ok;
    r.iconChecksum = icon.checksum;
}

void addRecord(const Entry& e) {
    Record r;
    fillRecord(r, e);
    sHistory.push_back(std::move(r));
    while (sHistory.size() > kHistoryMax) {
        sHistory.pop_front();
    }
    TwiliLog.info("[toast] queued {} #{} \"{}\"", kindName(e.kind), e.seq, entryText(e, false));
}

// Merges `who`'s pending item toasts (plus `extra`) into one at the first one's place.
void mergeSender(const Sender& who, const Entry* extra) {
    Entry merged;
    merged.kind = Kind::Merged;
    merged.senders = {who};
    std::vector<size_t> taken;
    for (size_t i = 0; i < sPending.size(); i++) {
        const Entry& q = sPending[i];
        if (q.kind == Kind::Summary || !sameSender(q.senders.front(), who)) {
            continue;
        }
        if (taken.empty()) {
            merged.queuedAt = q.queuedAt;
        }
        merged.items.insert(merged.items.end(), q.items.begin(), q.items.end());
        merged.total += q.total;
        if (Record* r = findRecord(q.seq)) {
            r->mergedAway = true;
            r->holdReason.clear();
        }
        taken.push_back(i);
    }
    if (taken.empty()) {
        return;
    }
    if (extra != nullptr) {
        merged.items.insert(merged.items.end(), extra->items.begin(), extra->items.end());
        merged.total += extra->total;
    }
    merged.seq = sNextSeq++;
    addRecord(merged);
    sPending[taken.front()] = std::move(merged);
    for (size_t k = taken.size(); k-- > 1;) {
        sPending.erase(sPending.begin() + static_cast<std::ptrdiff_t>(taken[k]));
    }
}

void enqueue(Entry e) {
    e.queuedAt = clock::now();
    if (e.kind == Kind::Item || e.kind == Kind::Own) {
        // A sender whose backlog already merged keeps adding to that toast.
        for (Entry& q : sPending) {
            if (q.kind == Kind::Merged && sameSender(q.senders.front(), e.senders.front())) {
                q.items.insert(q.items.end(), e.items.begin(), e.items.end());
                q.total += e.total;
                if (Record* r = findRecord(q.seq)) {
                    fillRecord(*r, q);
                }
                TwiliLog.info("[toast] merged into #{} \"{}\"", q.seq, entryText(q, false));
                return;
            }
        }
        if (sPending.size() >= kMaxPending) {
            const Sender& who = e.senders.front();
            const bool senderPending =
                std::any_of(sPending.begin(), sPending.end(), [&](const Entry& q) {
                    return q.kind != Kind::Summary && sameSender(q.senders.front(), who);
                });
            if (senderPending) {
                mergeSender(who, &e);
                return;
            }
            // Another sender's backlog makes room: the one with the most toasts waiting.
            std::map<uint64_t, int> counts;
            const Sender* busiest = nullptr;
            int most = 1;
            for (const Entry& q : sPending) {
                if (q.kind == Kind::Summary) {
                    continue;
                }
                const Sender& s = q.senders.front();
                const int n = ++counts[s.self ? ~0ull : s.clientId];
                if (n > most) {
                    most = n;
                    busiest = &s;
                }
            }
            if (busiest != nullptr) {
                const Sender other = *busiest;  // mergeSender rewrites the queue it points into
                mergeSender(other, nullptr);
            }
        }
    }
    e.seq = sNextSeq++;
    addRecord(e);
    sPending.push_back(std::move(e));
}

void enqueueItem(Kind kind, const Sender& who, const ItemCount& ic) {
    Entry e;
    e.kind = kind;
    e.senders = {who};
    e.items = {displayed(ic)};
    e.total = ic.count;
    enqueue(std::move(e));
}

void enqueueSummary(const std::vector<Sender>& senders, const std::vector<ItemCount>& items,
    int total) {
    Entry e;
    e.kind = Kind::Summary;
    e.senders = senders;
    for (const ItemCount& ic : items) {
        e.items.push_back(displayed(ic));
    }
    e.total = total;
    enqueue(std::move(e));
}

void touchWindow(clock::time_point now) {
    if (!sWindow.open) {
        sWindow = {};
        sWindow.open = true;
        sWindow.openedAt = now;
    }
    sWindow.lastActivity = now;
}

void countInWindow(const Sender& who, const ItemCount& ic) {
    auto it = std::find_if(sWindow.tallies.begin(), sWindow.tallies.end(),
        [&](const Tally& t) { return sameSender(t.who, who); });
    if (it == sWindow.tallies.end()) {
        sWindow.tallies.push_back({who});
        it = sWindow.tallies.end() - 1;
    }
    it->total += ic.count;
    it->items.push_back(ic);
    sWindow.arrivals.push_back(ic);
}

// A handful from one sender reads best as the items themselves; more becomes one summary.
void settle(const std::vector<Tally>& tallies, const std::vector<ItemCount>& arrivals) {
    int total = 0;
    for (const Tally& t : tallies) {
        total += t.total;
    }
    if (total == 0) {
        return;
    }
    if (tallies.size() == 1 && total <= kIndividualMax) {
        for (const ItemCount& ic : tallies[0].items) {
            enqueueItem(Kind::Item, tallies[0].who, ic);
        }
        return;
    }
    std::vector<Sender> senders;
    for (const Tally& t : tallies) {
        if (t.total > 0) {
            senders.push_back(t.who);
        }
    }
    enqueueSummary(senders, arrivals, total);
}

void closeWindow() {
    const CatchUpWindow window = std::move(sWindow);
    sWindow = {};
    TwiliLog.info("[toast] catch-up window closed: {} sender(s), {} item(s)",
        window.tallies.size(), window.arrivals.size());
    settle(window.tallies, window.arrivals);
}

bool teammateToastsOn() {
    return config::getBool(config::Var::ItemToasts);
}

bool ownToastsOn() {
    return config::getBool(config::Var::ItemToastsOwn);
}

bool readInt(const nlohmann::json& obj, const char* key, int& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return false;
    }
    out = std::clamp(it->get<int>(), 0, 255);
    return true;
}

}  // namespace

Sender senderOf(const Session& session, const nlohmann::json& packet) {
    Sender s;
    const auto id = packet.find("clientId");
    if (id != packet.end() && id->is_number_unsigned()) {
        s.clientId = id->get<uint32_t>();
    }
    const auto row = session.clients().find(s.clientId);
    if (row != session.clients().end() && !row->second.name.empty()) {
        s.name = row->second.name;
        s.r = row->second.colorR;
        s.g = row->second.colorG;
        s.b = row->second.colorB;
        s.hasColor = true;
        return s;
    }
    // Stamped by the relay (tools/server/server.js).
    const auto name = packet.find("senderName");
    if (name != packet.end() && name->is_string()) {
        s.name = name->get<std::string>();
        s.nameFromPacket = !s.name.empty();
    }
    const auto color = packet.find("senderColor");
    int r = 255, g = 255, b = 255;
    if (color != packet.end() && color->is_object() && readInt(*color, "r", r) &&
        readInt(*color, "g", g) && readInt(*color, "b", b))
    {
        s.r = static_cast<uint8_t>(r);
        s.g = static_cast<uint8_t>(g);
        s.b = static_cast<uint8_t>(b);
        s.hasColor = true;
    }
    return s;
}

bool isToastableItem(uint8_t itemNo) {
    return isProgressionItem(itemNo) || isDungeonItem(itemNo);
}

void noteItem(const Sender& who, uint8_t itemNo, int count, int saveTbl, Source src, bool applied) {
    if (!teammateToastsOn() || !isToastableItem(itemNo) || count <= 0) {
        return;
    }
    const ItemCount ic{itemNo, count, saveTbl};
    if (src == Source::Live) {
        enqueueItem(Kind::Item, who, ic);
        return;
    }
    // A reconnect replays the whole queue, and items we already have must not count.
    touchWindow(clock::now());
    if (applied) {
        countInWindow(who, ic);
    }
}

void noteMerge(const Sender& who, Source src, const std::vector<ItemCount>& items) {
    if (!teammateToastsOn()) {
        return;
    }
    std::vector<ItemCount> counted;
    for (const ItemCount& ic : items) {
        if (ic.count > 0 && isToastableItem(ic.itemNo)) {
            counted.push_back(ic);
        }
    }
    if (src == Source::CatchUpMerge || sWindow.open) {
        touchWindow(clock::now());
        for (const ItemCount& ic : counted) {
            countInWindow(who, ic);
        }
        return;
    }
    // Nothing to summarize once its GIVE_ITEMs landed, unless gained outside a give or apart.
    Tally t{who};
    for (const ItemCount& ic : counted) {
        t.total += ic.count;
        t.items.push_back(ic);
    }
    settle({t}, counted);
}

void noteOwnItem(uint8_t itemNo, int saveTbl) {
    if (!ownToastsOn() || !isToastableItem(itemNo)) {
        return;
    }
    Sender self;
    self.self = true;
    self.name = config::getString(config::Var::DisplayName);
    const auto color = localPlayerColor();
    self.r = static_cast<uint8_t>(color[0]);
    self.g = static_cast<uint8_t>(color[1]);
    self.b = static_cast<uint8_t>(color[2]);
    self.hasColor = true;
    enqueueItem(Kind::Own, self, {itemNo, 1, isDungeonItem(itemNo) ? saveTbl : -1});
}

void beginCatchUp() {
    touchWindow(clock::now());
    TwiliLog.info("[toast] catch-up window open");
}

void tick() {
    const auto now = clock::now();
    if (sWindow.open) {
        const auto sinceOpen = now - sWindow.openedAt;
        if (sinceOpen >= kWindowMax ||
            (now - sWindow.lastActivity >= kWindowQuiet && sinceOpen >= kWindowMin))
        {
            closeWindow();
        }
    }

    // Turning a setting off drops what it would still show.
    const bool teammates = teammateToastsOn();
    const bool own = ownToastsOn();
    std::erase_if(sPending,
        [&](const Entry& e) { return e.kind == Kind::Own ? !own : !teammates; });
    if (sPending.empty()) {
        return;
    }

    // Fine over a pause screen, not over a loading screen or a cutscene (item-get included).
    const char* block = local::localTeleportBlocker();
    const bool hold = block != nullptr &&
                      (std::strcmp(block, "not-in-game") == 0 ||
                          std::strcmp(block, "loading") == 0 || std::strcmp(block, "cutscene") == 0);
    for (const Entry& e : sPending) {
        if (Record* r = findRecord(e.seq)) {
            r->holdReason = hold ? block : "";
        }
    }
    if (hold || now < sOverlayBusyUntil) {
        return;
    }

    std::vector<Sender> stale;
    for (const Entry& e : sPending) {
        if (e.kind != Kind::Summary && now - e.queuedAt >= kStaleAfter &&
            std::none_of(stale.begin(), stale.end(),
                [&](const Sender& s) { return sameSender(s, e.senders.front()); }))
        {
            stale.push_back(e.senders.front());
        }
    }
    for (const Sender& who : stale) {
        const auto n = std::count_if(sPending.begin(), sPending.end(), [&](const Entry& e) {
            return e.kind != Kind::Summary && sameSender(e.senders.front(), who);
        });
        if (n >= 2) {
            mergeSender(who, nullptr);
        }
    }

    Entry e = std::move(sPending.front());
    sPending.pop_front();
    const bool single = e.kind == Kind::Item || e.kind == Kind::Own;
    const auto duration = !single               ? kSummaryDuration
                          : sPending.size() >= 2 ? kBusyItemDuration
                                                 : kItemDuration;
    const char* title = e.kind == Kind::Summary ? "Items synced" : single ? "Item" : "Items";
    ui::toastRml(fmt::format("<toast-title>{}</toast-title>", title), entryRml(e), ui::kToastItem,
        static_cast<uint32_t>(duration.count()));
    sOverlayBusyUntil = now + duration + kToastFade;
    if (Record* r = findRecord(e.seq)) {
        r->pushed = true;
        r->holdReason.clear();
    }
    TwiliLog.info("[toast] {} #{} \"{}\"",
        e.kind == Kind::Summary ? "summary" : e.kind == Kind::Merged ? "merged" : "shown", e.seq,
        entryText(e, false));
}

void resetSession() {
    sPending.clear();
    sWindow = {};
}

IconProbe iconProbe(uint8_t itemNo) {
    return iconInfo(itemNo);
}

void clearIconCache() {
    sIcons = {};
}

const std::deque<Record>& history() {
    return sHistory;
}

size_t pendingCount() {
    return sPending.size();
}

void clearForTest() {
    sPending.clear();
    sHistory.clear();
    sWindow = {};
    sOverlayBusyUntil = {};
}

}  // namespace twili::item_toasts
