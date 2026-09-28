#include "game/GameIdentity.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "game/RandoSeeds.hpp"
#include "game/TeamGame.hpp"

#include <mods/items.h>
#include <mods/svc/host.h>
#include <mods/svc/item.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>

namespace twili::game_identity {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kRandomizerModeId = "randomizer_dev.twilitrealm.randomizer";
constexpr const char* kRandomizerModId = "dev.twilitrealm.randomizer";
constexpr const char* kUnverifiedPrefix = "rando-probe/";
constexpr auto kProbeInterval = std::chrono::seconds(2);
constexpr auto kPublishInterval = std::chrono::milliseconds(250);
constexpr uint8_t kProbeSentinel = 0xFF;
// What the randomizer gives instead of ammo the player has no container for yet.
constexpr uint8_t kBlueRupee = 0x02;
// A seed file counts as the running seed only if this many of its checks answer alike.
constexpr int kMinProbeMatches = 8;

// Checks every build answers by name: what resolves them changes with the loaded seed.
constexpr const char* kProbeChecks[] = {
    ITEM_CHECK_ARCHERY_REWARD,
    ITEM_CHECK_ASHEI_SKETCH,
    ITEM_CHECK_AURU_MEMO,
    ITEM_CHECK_BALL_AND_CHAIN,
    ITEM_CHECK_BULBLIN_KEY,
    ITEM_CHECK_CORAL_EARRING,
    ITEM_CHECK_CORO_BOTTLE,
    ITEM_CHECK_CORO_GATE_KEY,
    ITEM_CHECK_CORO_LANTERN,
    ITEM_CHECK_DUNGEON_MAP_SNOWPEAK,
    ITEM_CHECK_FAIRY_REWARD,
    ITEM_CHECK_FISHING_BOTTLE,
    ITEM_CHECK_FISHING_HEART_PIECE,
    ITEM_CHECK_GOATS_REWARD,
    ITEM_CHECK_GORON_REWARD,
    ITEM_CHECK_GORON_SPRINGWATER_RUSH,
    ITEM_CHECK_ILIA_CHARM,
    ITEM_CHECK_ILIA_MEMORY,
    ITEM_CHECK_IZA_REWARD_1,
    ITEM_CHECK_IZA_REWARD_2,
    ITEM_CHECK_JOVANI_REWARD_1,
    ITEM_CHECK_JOVANI_REWARD_2,
    ITEM_CHECK_KEY_SHARD_1,
    ITEM_CHECK_KEY_SHARD_2,
    ITEM_CHECK_KEY_SHARD_3,
    ITEM_CHECK_ORDON_SHIELD,
    ITEM_CHECK_ORDON_SWORD,
    ITEM_CHECK_PLUMM_REWARD,
    ITEM_CHECK_PRAYER_REWARD,
    ITEM_CHECK_RENADO_LETTER,
    ITEM_CHECK_SERA_REWARD,
    ITEM_CHECK_SHAD_DOMINION_ROD,
    ITEM_CHECK_SKYBOOK,
    ITEM_CHECK_SNOWBOARD_REWARD,
    ITEM_CHECK_STAR_REWARD_1,
    ITEM_CHECK_STAR_REWARD_2,
    ITEM_CHECK_TELMA_INVOICE,
    ITEM_CHECK_ULI_CRADLE_REWARD,
    ITEM_CHECK_WOOD_STATUE,
    ITEM_CHECK_ZORA_ARMOR,
    ITEM_CHECK_DUNGEON_REWARD_FOREST,
    ITEM_CHECK_DUNGEON_REWARD_GORON,
    ITEM_CHECK_DUNGEON_REWARD_LAKEBED,
    ITEM_CHECK_DUNGEON_REWARD_ARBITERS,
    ITEM_CHECK_DUNGEON_REWARD_SNOWPEAK,
    ITEM_CHECK_DUNGEON_REWARD_TIME,
    ITEM_CHECK_DUNGEON_REWARD_CITY,
    ITEM_CHECK_MASTER_SWORD,
    ITEM_CHECK_SHADOW_CRYSTAL,
};

struct State {
    Identity current;
    bool detected = false;
    bool dirty = true;
    std::string mode;
    bool inGame = false;
    uint32_t saveGeneration = 0;
    std::string fingerprint;
    bool anyResolved = false;
    Clock::time_point nextProbe{};
    // Last body the server got; null before the handshake.
    nlohmann::json sent;
    Clock::time_point sentAt{};
    bool forcePublish = false;
#if TWILI_ENABLE_AUTOTEST
    std::optional<nlohmann::json> override;
#endif
};

State s_state;

uint8_t probe(const char* name) {
    uint8_t out = kProbeSentinel;
    if (svc_item == nullptr ||
        svc_item->resolve_check(mod_ctx, name, kProbeSentinel, &out) != MOD_OK)
    {
        return kProbeSentinel;
    }
    return out;
}

// Inventory-dependent answers count only as "some item", so the fingerprint holds across a run.
void probeFingerprint() {
    State& st = s_state;
    std::string text;
    st.anyResolved = false;
    for (const char* name : kProbeChecks) {
        const uint8_t item = probe(name);
        st.anyResolved |= item != kProbeSentinel;
        text += fmt::format("{}={};", name,
            rando_seeds::inventoryDependent(item) || item == kBlueRupee ?
                std::string("v") :
                fmt::format("{:02x}", item));
    }
    st.fingerprint = rando_seeds::fnv64Hex(text);
}

fs::path randomizerSeedsDir() {
    const char* dir = nullptr;
    if (!SERVICE_HAS(svc_host, HostService, data_dir) ||
        svc_host->data_dir(mod_ctx, &dir) != MOD_OK || dir == nullptr)
    {
        return {};
    }
    fs::path ours(reinterpret_cast<const char8_t*>(dir));
    if (!ours.has_filename()) {
        ours = ours.parent_path();
    }
    return ours.parent_path() / kRandomizerModId / "seeds";
}

// The generated seed whose checks the running resolver answers exactly like, if any.
const rando_seeds::Seed* findRunningSeed() {
    const fs::path dir = randomizerSeedsDir();
    if (dir.empty()) {
        return nullptr;
    }
    for (const rando_seeds::Seed& seed : rando_seeds::scan(dir)) {
        int compared = 0;
        int matched = 0;
        for (const auto& [name, item] : seed.locations) {
            if (rando_seeds::inventoryDependent(item)) {
                continue;
            }
            ++compared;
            if (probe(name.c_str()) == item) {
                ++matched;
            }
        }
        if (compared >= kMinProbeMatches && matched == compared) {
            return &seed;
        }
    }
    return nullptr;
}

std::string shortVersion(const std::string& full) {
    // "v1.0.5-HEAD-275ed87" -> "1.0.5"
    std::string v = full.rfind('v', 0) == 0 ? full.substr(1) : full;
    const auto dash = v.find('-');
    return dash == std::string::npos ? v : v.substr(0, dash);
}

bool keyChar(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '/' || ch == '.' || ch == '_' || ch == '-';
}

Identity detect() {
    State& st = s_state;
    Identity id;
    id.inGame = st.inGame;
    const bool randomizer = st.mode == kRandomizerModeId || (st.mode.empty() && st.anyResolved);
    if (st.mode.empty() && !randomizer) {
        id.kind = "vanilla";
        id.key = "vanilla";
        id.mode = "Vanilla";
    } else if (st.mode == "vanilla" || st.mode == "vanilla_speedrun") {
        id.kind = "vanilla";
        id.key = "vanilla";
        id.mode = st.mode == "vanilla" ? "Vanilla" : "Vanilla (speedrun)";
    } else if (randomizer) {
        id.kind = "randomizer";
        id.mode = "Randomizer";
        if (!id.inGame) {
            return id;
        }
        if (const rando_seeds::Seed* seed = findRunningSeed()) {
            id.key = fmt::format("rando/f{}/{}", seed->formatVersion, seed->digest);
            id.name = seed->hash;
            if (!seed->version.empty()) {
                id.mode = "Randomizer " + shortVersion(seed->version);
            }
            id.permalink = seed->permalink;
            id.seed = seed->seedString;
            id.version = seed->version;
        } else {
            id.key = kUnverifiedPrefix + st.fingerprint;
            id.verified = false;
            id.name = st.anyResolved ? "unknown seed" : "no seed loaded";
        }
    } else {
        id.kind = "mode";
        id.mode = st.mode;
        std::string key = "mode/";
        for (const char ch : st.mode) {
            key += keyChar(ch) ? ch : '_';
        }
        id.key = key.substr(0, 96);
    }
    if (!id.inGame) {
        id.key.clear();
    }
    return id;
}

#if TWILI_ENABLE_AUTOTEST
Identity fromOverride(const nlohmann::json& j) {
    Identity id;
    id.inGame = j.value("inGame", s_state.inGame);
    id.kind = j.value("kind", std::string("randomizer"));
    id.key = id.inGame ? j.value("key", std::string{}) : std::string{};
    id.name = j.value("name", std::string{});
    id.mode =
        j.value("mode", id.kind == "vanilla" ? std::string("Vanilla") : std::string("Randomizer"));
    id.verified = !isUnverifiedKey(id.key);
    id.permalink = j.value("permalink", std::string{});
    id.seed = j.value("seed", std::string{});
    id.version = j.value("version", std::string{});
    return id;
}
#endif

void refresh() {
    State& st = s_state;
    st.dirty = false;
    Identity id;
#if TWILI_ENABLE_AUTOTEST
    if (st.override.has_value()) {
        id = fromOverride(*st.override);
    } else
#endif
    {
        id = detect();
    }
    const bool changed = !st.detected || id.inGame != st.current.inGame ||
                         id.key != st.current.key || id.kind != st.current.kind ||
                         id.name != st.current.name || id.permalink != st.current.permalink;
    st.current = std::move(id);
    st.detected = true;
    if (changed) {
        const Identity& c = st.current;
        TwiliLog.info("[game] local game: {} {}{}{}", c.kind, c.inGame ? c.key : "(not in game)",
            c.name.empty() ? "" : " \"" + c.name + "\"",
            c.inGame && c.kind == "randomizer" ?
                (c.verified ? (c.permalink.empty() ? ", no permalink" : ", permalink known") :
                              ", unverified") :
                "");
    }
}

nlohmann::json body() {
    const Identity& id = s_state.current;
    nlohmann::json j = {
        {"inGame", id.inGame},
        {"kind", id.kind},
        {"key", id.inGame ? id.key : std::string{}},
        {"display", {{"name", id.name}, {"mode", id.mode}}},
        {"allowUnverified", team_game::unverifiedAllowed()},
    };
    if (id.inGame && (!id.permalink.empty() || !id.seed.empty() || !id.version.empty())) {
        j["share"] = {{"permalink", id.permalink}, {"seed", id.seed}, {"version", id.version}};
    }
    return j;
}

void sense(Clock::time_point now) {
    State& st = s_state;
    const bool inGame = isSaveLoaded();
    const uint32_t generation = saveGeneration();
    if (inGame != st.inGame || generation != st.saveGeneration) {
        st.inGame = inGame;
        st.saveGeneration = generation;
        st.dirty = true;
        st.nextProbe = {};
    }
    if (now < st.nextProbe) {
        return;
    }
    st.nextProbe = now + kProbeInterval;
    const std::string mode = config::hostString("game.lastSelectedGameModeId", std::string{});
    if (mode != st.mode) {
        st.mode = mode;
        st.dirty = true;
    }
    if (st.inGame && (st.mode.empty() || st.mode == kRandomizerModeId)) {
        const std::string before = st.fingerprint;
        probeFingerprint();
        st.dirty |= st.fingerprint != before;
    }
}

}  // namespace

const Identity& current() {
    return s_state.current;
}

bool isUnverifiedKey(const std::string& key) {
    return key.rfind(kUnverifiedPrefix, 0) == 0;
}

nlohmann::json handshakeJson() {
    State& st = s_state;
    sense(Clock::now());
    if (st.dirty || !st.detected) {
        refresh();
    }
    st.sent = body();
    st.sentAt = Clock::now();
    return st.sent;
}

void tick() {
    State& st = s_state;
    const auto now = Clock::now();
    sense(now);
    if (st.dirty) {
        refresh();
    }
    if (!Session::active() || !Session::instance().joined() || st.sent.is_null()) {
        return;
    }
    nlohmann::json next = body();
    if ((next == st.sent && !st.forcePublish) || now - st.sentAt < kPublishInterval) {
        return;
    }
    st.forcePublish = false;
    st.sent = next;
    st.sentAt = now;
    next["type"] = "GAME_IDENTITY";
    Session::instance().send(next);
}

void invalidate() {
    s_state.dirty = true;
    s_state.nextProbe = {};
}

void republish() {
    s_state.forcePublish = true;
}

void resetSession() {
    s_state.sent = nullptr;
    s_state.forcePublish = false;
}

#if TWILI_ENABLE_AUTOTEST
void setOverrideForTest(const nlohmann::json& identity) {
    if (identity.is_object() && !identity.empty()) {
        s_state.override = identity;
    } else {
        s_state.override.reset();
    }
    invalidate();
}
#endif

}  // namespace twili::game_identity
