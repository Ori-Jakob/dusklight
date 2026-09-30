// Steps for item toasts, checked through their history; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "game/GameIdentity.hpp"
#include "ui/ItemNames.hpp"
#include "ui/ItemToasts.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace toasts = item_toasts;

bool validUtf8Printable(const std::string& s) {
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size() || (len == 1 && c < 0x20)) {
            return false;
        }
        for (int k = 1; k < len; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        any = any || c != ' ';
        i += len;
    }
    return any;
}

std::string iconSelfTest(const std::vector<int>& items) {
    std::map<uint32_t, int> seen;
    std::map<int, uint32_t> first;
    for (const int item : items) {
        const auto no = static_cast<uint8_t>(item);
        const toasts::IconProbe px = toasts::iconProbe(no);
        const std::string& name = ui::itemDisplayName(no);
        const ui::ItemNameSource source = ui::itemNameSource(no);
        const bool fromGame = source == ui::ItemNameSource::Game;
        TwiliLog.info("[toast] icon 0x{:02X} {}x{} layers={} crc={:08x} name=\"{}\" fromGame={}",
            item, px.width, px.height, px.layers, px.checksum, name, fromGame);
        if (!px.ok || px.width < 16 || px.width > 64 || px.height < 16 || px.height > 64) {
            return fmt::format("item 0x{:02X}: icon {}x{}", item, px.width, px.height);
        }
        if (item == 0x60 && px.layers < 2) {
            return fmt::format("bottle: {} layer(s), no glass", px.layers);
        }
        const auto [it, fresh] = seen.emplace(px.checksum, item);
        if (!fresh) {
            return fmt::format("items 0x{:02X} and 0x{:02X} have the same icon", it->second, item);
        }
        // The game's text, or our fixed name where it reads wrong (the heart piece's plural).
        if (!validUtf8Printable(name) || (!fromGame && source != ui::ItemNameSource::Mod)) {
            return fmt::format("item 0x{:02X}: name \"{}\" (from game: {})", item, name, fromGame);
        }
        first[item] = px.checksum;
    }
    toasts::clearIconCache();
    for (const int item : items) {
        if (toasts::iconProbe(static_cast<uint8_t>(item)).checksum != first[item]) {
            return fmt::format("item 0x{:02X}: checksum changed on reload", item);
        }
    }
    return {};
}

// Ids the randomizer hands out, progressive steps included (mods/randomizer generator items.yaml).
constexpr uint8_t kRandoItems[] = {0x14, 0x15, 0x21, 0x22, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x30, 0x31,
    0x32, 0x35, 0x36, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x52, 0x55, 0x56, 0x57, 0x60,
    0x65, 0x75, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D,
    0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9D, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB,
    0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB,
    0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEE, 0xF3, 0xF4, 0xF5,
    0xF6, 0xF9, 0xFA, 0xFD, 0xFE};

// Ids the unmodded game never uses (its NOENTRY items).
bool unusedInVanilla(int no) {
    return (no >= 0x13 && no <= 0x15) || (no >= 0x37 && no <= 0x3C) || no == 0x4D || no == 0x4E ||
           no == 0x52 || no == 0x57 || (no >= 0x85 && no <= 0x8F) || (no >= 0x92 && no <= 0x9B) ||
           (no >= 0xA8 && no <= 0xAF) || (no >= 0xB6 && no <= 0xBF) || (no >= 0xD8 && no <= 0xDF) ||
           (no >= 0xE1 && no <= 0xE8);
}

// Every toastable item this game can hand out has a real name: no placeholder, number or bare noun.
std::string nameAudit(std::vector<int> items, bool rando) {
    if (items.empty()) {
        if (rando) {
            items.assign(std::begin(kRandoItems), std::end(kRandoItems));
        } else {
            for (int no = 0; no < 0xFF; no++) {
                if (!unusedInVanilla(no)) {
                    items.push_back(no);
                }
            }
        }
    }
    int checked = 0;
    for (const int item : items) {
        const auto no = static_cast<uint8_t>(item);
        if (!toasts::isToastableItem(no)) {
            continue;
        }
        checked++;
        const std::string& name = ui::itemDisplayName(no);
        const ui::ItemNameSource source = ui::itemNameSource(no);
        TwiliLog.info("[toast] name 0x{:02X} \"{}\" source={}", item, name, static_cast<int>(source));
        if (source == ui::ItemNameSource::Unknown || !validUtf8Printable(name) ||
            name.find("Reserved") != std::string::npos || name.rfind("item 0x", 0) == 0 ||
            (name[0] >= 'a' && name[0] <= 'z'))
        {
            return fmt::format("item 0x{:02X}: name \"{}\"", item, name);
        }
    }
    TwiliLog.info("[toast] name audit: {} item(s) ok", checked);
    return {};
}

bool recordMatches(const toasts::Record& r, const json& step, bool defaultPushed) {
    if (step.contains("kind") && r.kind != step.value("kind", std::string{})) {
        return false;
    }
    if (step.contains("from")) {
        const std::string from = step.value("from", std::string{});
        if (std::find(r.senderNames.begin(), r.senderNames.end(), from) == r.senderNames.end()) {
            return false;
        }
    }
    if (step.contains("item")) {
        const auto item = static_cast<uint8_t>(step.value("item", -1));
        if (std::find(r.items.begin(), r.items.end(), item) == r.items.end()) {
            return false;
        }
    }
    if (step.contains("textContains") &&
        r.text.find(step.value("textContains", std::string{})) == std::string::npos)
    {
        return false;
    }
    if (step.contains("minCount") && r.count < step.value("minCount", 0)) {
        return false;
    }
    if (step.contains("fromPacket") && r.nameFromPacket != step.value("fromPacket", true)) {
        return false;
    }
    if (step.contains("hold") && r.holdReason != step.value("hold", std::string{})) {
        return false;
    }
    if (step.contains("mergedAway") && r.mergedAway != step.value("mergedAway", false)) {
        return false;
    }
    if (step.contains("pushed") || defaultPushed) {
        if (r.pushed != step.value("pushed", true)) {
            return false;
        }
    }
    return true;
}

json recordJson(const toasts::Record& r) {
    return {{"seq", r.seq}, {"kind", r.kind}, {"senderId", r.senderId},
        {"senders", r.senderNames}, {"fromPacket", r.nameFromPacket}, {"items", r.items},
        {"count", r.count}, {"text", r.text}, {"iconOk", r.iconOk}, {"iconCrc", r.iconChecksum},
        {"pushed", r.pushed}, {"mergedAway", r.mergedAway}, {"hold", r.holdReason}};
}

json historyJson() {
    json all = json::array();
    for (const toasts::Record& r : toasts::history()) {
        all.push_back(recordJson(r));
    }
    return all;
}

// The body the overlay got shows that item's icon from the item:// provider.
bool showsIcon(const toasts::Record& r, uint8_t item) {
    return r.pushed && r.rml.find(fmt::format("item://item/{:02x}", item)) != std::string::npos;
}

std::optional<bool> itemToastSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "itemToastSelfTest") {
        const std::vector<int> items = step.value("items",
            std::vector<int>{0x40, 0x41, 0x43, 0x44, 0x45, 0x46, 0x48, 0x4B, 0x20, 0x21, 0x26, 0x60,
                0xF6, 0xFD});
        const std::string err = iconSelfTest(items);
        if (!err.empty()) {
            ctx.fail("itemToastSelfTest: " + err);
            return false;
        }
        TwiliLog.info("[toast] self-test passed");
        return true;
    }

    if (op == "itemNameAudit") {
        const bool rando = step.value("rando", game_identity::current().kind == "randomizer");
        const std::string err = nameAudit(step.value("items", std::vector<int>{}), rando);
        if (!err.empty()) {
            ctx.fail("itemNameAudit: " + err);
            return false;
        }
        return true;
    }

    if (op == "itemToastOption") {
        const std::string name = step.value("name", std::string{});
        const bool value = step.value("value", true);
        if (name == "item_toasts") {
            config::setBool(config::Var::ItemToasts, value);
        } else if (name == "item_toasts_own") {
            config::setBool(config::Var::ItemToastsOwn, value);
        } else {
            ctx.fail("itemToastOption: unknown option '" + name + "'");
            return false;
        }
        return true;
    }

    if (op == "clearItemToasts") {
        toasts::clearForTest();
        return true;
    }

    if (op == "testItemToast") {
        toasts::Sender who;
        who.clientId = 0xFFFF;
        who.name = step.value("name", std::string("Tester"));
        who.r = static_cast<uint8_t>(step.value("r", 255));
        who.g = static_cast<uint8_t>(step.value("g", 255));
        who.b = static_cast<uint8_t>(step.value("b", 255));
        who.hasColor = true;
        toasts::noteItem(who, static_cast<uint8_t>(step.value("item", 0x44)),
            step.value("count", 1), step.value("saveTbl", -1), toasts::Source::Live, true);
        return true;
    }

    if (op == "expectItemToast") {
        const bool rendered = step.value("rendered", false);
        for (const toasts::Record& r : toasts::history()) {
            if (r.mergedAway || !recordMatches(r, step, true)) {
                continue;
            }
            if (rendered) {
                // A summary shows only its most recent items: any icon of it will do.
                std::vector<uint8_t> icons = r.items;
                if (step.contains("item")) {
                    icons = {static_cast<uint8_t>(step.value("item", 0))};
                }
                if (!r.iconOk || std::none_of(icons.begin(), icons.end(),
                                     [&](uint8_t item) { return showsIcon(r, item); }))
                {
                    continue;
                }
            }
            TwiliLog.info("[autotest] item toast #{} \"{}\" pushed={} icon={} crc={:08x}", r.seq,
                r.text, r.pushed, r.iconOk, r.iconChecksum);
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format(
                "expectItemToast {} never matched; history {}", step.dump(), historyJson().dump()));
        }
        return false;
    }

    if (op == "expectNoItemToast") {
        for (const toasts::Record& r : toasts::history()) {
            if (recordMatches(r, step, false)) {
                ctx.fail(fmt::format(
                    "expectNoItemToast {}: got {}", step.dump(), recordJson(r).dump()));
                return false;
            }
        }
        return ctx.seconds >= step.value("forSec", 5.0);
    }

    if (op == "expectItemToastCount") {
        int n = 0;
        for (const toasts::Record& r : toasts::history()) {
            n += recordMatches(r, step, false) ? 1 : 0;
        }
        const bool ok = step.contains("count")
                            ? n == step.value("count", 0)
                            : n >= step.value("min", 0) && n <= step.value("max", 1 << 30);
        if (!ok) {
            ctx.fail(fmt::format("expectItemToastCount {}: {} match; history {}", step.dump(), n,
                historyJson().dump()));
            return false;
        }
        return true;
    }

    if (op == "expectItemToastItems") {
        const std::string from = step.value("from", std::string{});
        int total = 0;
        for (const toasts::Record& r : toasts::history()) {
            if (r.mergedAway ||
                std::find(r.senderNames.begin(), r.senderNames.end(), from) == r.senderNames.end())
            {
                continue;
            }
            total += r.count;
        }
        if (total != step.value("count", 0)) {
            ctx.fail(fmt::format("expectItemToastItems {}: {} item(s)", step.dump(), total));
            return false;
        }
        return true;
    }

    if (op == "dumpItemToasts") {
        for (const toasts::Record& r : toasts::history()) {
            TwiliLog.info("[toast] record {}", recordJson(r).dump());
        }
        TwiliLog.info("[toast] {} pending", toasts::pendingCount());
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&itemToastSteps);

}  // namespace
}  // namespace twili::autotest
