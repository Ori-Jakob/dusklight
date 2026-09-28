// Steps for item toasts, checked through their history; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "ui/ItemNames.hpp"
#include "ui/ItemToasts.hpp"

#include <fmt/format.h>

#include <algorithm>
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
        const bool fromGame = ui::itemNameFromGame(no);
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
        if (!validUtf8Printable(name) || (!fromGame && no != 0x21)) {
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
