#include "game/RandoSeeds.hpp"

#include "core/Log.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace twili::rando_seeds {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxSeeds = 128;
constexpr uintmax_t kMaxSeedFileBytes = 16u << 20;

// seed.dat sections that decide placements; hint and message text may differ between builds.
constexpr std::string_view kDigestSections[] = {
    "formatVersion",
    "mSettings",
    "mStartEventFlags",
    "mStartRegionFlags",
    "mStartingInventory",
    "mTreasureChestOverrides",
    "mPoeOverrides",
    "mFreestandingItemOverrides",
    "mBugRewardOverrides",
    "mSkyCharacterOverrides",
    "mGoldenWolfOverrides",
    "mShopOverrides",
    "mTwilitInsectOverrides",
    "mItemLocations",
    "mStartHour",
    "mMapBits",
    "mEntranceOverrides",
    "mReturnToPlaceOverrides",
};

// verifyProgressiveItem's cases in the randomizer (swords, bows, wallets, skills, clawshots,
// sky book, key shards, rods, mirror and shadow pieces, ammo).
constexpr uint8_t kInventoryDependent[] = {
    0x0A,
    0x0B,
    0x0C,
    0x0D,
    0x0E,
    0x0F,
    0x10,
    0x12,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1A,
    0x1B,
    0x1C,
    0x1D,
    0x28,
    0x29,
    0x35,
    0x36,
    0x3D,
    0x3F,
    0x43,
    0x44,
    0x46,
    0x47,
    0x49,
    0x4A,
    0x4C,
    0x55,
    0x56,
    0xA5,
    0xA6,
    0xA7,
    0xD8,
    0xD9,
    0xDA,
    0xDB,
    0xE1,
    0xE2,
    0xE3,
    0xE4,
    0xE5,
    0xE6,
    0xE7,
    0xE9,
    0xEA,
    0xEB,
    0xF9,
    0xFA,
    0xFD,
};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

size_t indentOf(std::string_view line) {
    const auto first = line.find_first_not_of(' ');
    return first == std::string_view::npos ? line.size() : first;
}

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) {
                ++i;
            }
            out += s[i];
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            out += s[i];
            if (s[i] == '\'' && i + 2 < s.size() && s[i + 1] == '\'') {
                ++i;
            }
        }
        return out;
    }
    return std::string(s);
}

// "key: value" or "key:" in yaml-cpp's block output; quoted keys may hold ": ".
bool splitKey(std::string_view s, std::string& key, std::string_view& value) {
    size_t colon = std::string_view::npos;
    if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
        const char quote = s.front();
        size_t i = 1;
        for (; i < s.size(); ++i) {
            if (quote == '"' && s[i] == '\\') {
                ++i;
            } else if (s[i] == quote) {
                if (quote == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
                    ++i;
                    continue;
                }
                break;
            }
        }
        if (i + 1 >= s.size() || s[i + 1] != ':') {
            return false;
        }
        key = unquote(s.substr(0, i + 1));
        colon = i + 1;
    } else {
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) {
                colon = i;
                break;
            }
        }
        if (colon == std::string_view::npos) {
            return false;
        }
        key = std::string(trim(s.substr(0, colon)));
    }
    value = trim(s.substr(colon + 1));
    return true;
}

bool parseInt(std::string_view s, int& out) {
    s = trim(s);
    const auto result = std::from_chars(s.data(), s.data() + s.size(), out);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size();
}

// "{itemId: 40, stage: 0, flag: 12}" when yaml-cpp chose the flow style.
bool flowItemId(std::string_view value, int& out) {
    const auto pos = value.find("itemId:");
    if (pos == std::string_view::npos) {
        return false;
    }
    std::string_view rest = value.substr(pos + 7);
    const auto end = rest.find_first_of(",}");
    return parseInt(rest.substr(0, end), out);
}

bool inDigest(std::string_view section) {
    return std::find(std::begin(kDigestSections), std::end(kDigestSections), section) !=
           std::end(kDigestSections);
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool readFile(const fs::path& path, std::string& out) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxSeedFileBytes) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = std::move(buffer).str();
    return true;
}

struct CacheEntry {
    fs::file_time_type seedTime{};
    uintmax_t seedSize = 0;
    fs::file_time_type logTime{};
    bool ok = false;
    Seed seed;
};

std::map<std::string, CacheEntry> s_cache;
std::vector<Seed> s_seeds;

}  // namespace

bool parseSeedData(std::string_view text, Seed& out) {
    std::vector<std::string> entries;
    std::string section;
    std::string parent;
    size_t base = std::string_view::npos;
    bool digestSection = false;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;

        const std::string_view body = trim(line);
        if (body.empty() || body.front() == '#' || body == "---" || body == "...") {
            continue;
        }
        const size_t indent = indentOf(line);
        std::string key;
        std::string_view value;
        std::string entry;

        if (indent == 0 && body.front() != '-') {
            section.clear();
            parent.clear();
            base = std::string_view::npos;
            if (!splitKey(body, key, value)) {
                continue;
            }
            section = key;
            digestSection = inDigest(section);
            if (section == "formatVersion") {
                int version = 0;
                out.formatVersion = parseInt(value, version) && version > 0 ? version : 0;
            }
            if (digestSection && !value.empty() && value != "{}" && value != "[]") {
                entries.push_back(section + "=" + std::string(value));
            }
            continue;
        }
        if (section.empty()) {
            continue;
        }
        if (base == std::string_view::npos) {
            base = indent;
        }

        if (body.front() == '-') {
            const std::string item(trim(body.substr(1)));
            entry = parent.empty() || indent < base ? section + "|-=" + item :
                                                      section + "|" + parent + "|-=" + item;
        } else if (!splitKey(body, key, value)) {
            continue;
        } else if (indent <= base) {
            if (value.empty()) {
                parent = key;
                continue;
            }
            parent.clear();
            entry = section + "|" + key + "=" + std::string(value);
            int item = 0;
            if (section == "mItemLocations" && flowItemId(value, item)) {
                out.locations.emplace_back(key, item);
            }
        } else if (!parent.empty()) {
            entry = section + "|" + parent + "|" + key + "=" + std::string(value);
            int item = 0;
            if (section == "mItemLocations" && key == "itemId" && parseInt(value, item)) {
                out.locations.emplace_back(parent, item);
            }
        }
        if (digestSection && !entry.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    if (out.formatVersion == 0 || out.locations.empty()) {
        return false;
    }
    std::sort(entries.begin(), entries.end());
    std::string canonical;
    for (const std::string& e : entries) {
        canonical += e;
        canonical += '\n';
    }
    out.digest = fnv64Hex(canonical);
    std::sort(out.locations.begin(), out.locations.end());
    return true;
}

void parseAntiSpoilerLog(std::string_view text, Seed& out) {
    const auto field = [&](std::string_view prefix) -> std::string {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            const std::string_view line = trim(text.substr(pos, end - pos));
            if (line.substr(0, prefix.size()) == prefix) {
                return std::string(trim(line.substr(prefix.size())));
            }
            pos = end + 1;
        }
        return {};
    };
    out.version = field("Dusklight Randomizer Version:");
    out.seedString = field("# Seed:");
    out.permalink = field("Permalink:");
}

const std::vector<Seed>& scan(const fs::path& seedsDir) {
    s_seeds.clear();
    std::error_code ec;
    if (!fs::is_directory(seedsDir, ec)) {
        s_cache.clear();
        return s_seeds;
    }
    std::set<std::string> seen;
    size_t folders = 0;
    for (fs::directory_iterator it(seedsDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || ++folders > kMaxSeeds) {
            continue;
        }
        const fs::path folder = it->path();
        const std::string hash = utf8(folder.filename());
        const fs::path dat = folder / "seed.dat";
        fs::path log = folder / folder.filename();
        log += " Anti-Spoiler Log.txt";
        std::error_code e1, e2, e3;
        const auto seedTime = fs::last_write_time(dat, e1);
        const auto seedSize = fs::file_size(dat, e2);
        auto logTime = fs::last_write_time(log, e3);
        if (e1 || e2) {
            continue;
        }
        if (e3) {
            logTime = {};
        }
        seen.insert(hash);
        CacheEntry& cached = s_cache[hash];
        if (cached.seedTime != seedTime || cached.seedSize != seedSize ||
            cached.logTime != logTime || cached.seed.hash.empty())
        {
            cached = CacheEntry{seedTime, seedSize, logTime};
            cached.seed.hash = hash;
            std::string text;
            cached.ok = readFile(dat, text) && parseSeedData(text, cached.seed);
            if (cached.ok && !e3 && readFile(log, text)) {
                parseAntiSpoilerLog(text, cached.seed);
            }
            TwiliLog.info("[game] seed \"{}\": {}", hash,
                cached.ok ?
                    fmt::format("{} name lookups, digest {}{}", cached.seed.locations.size(),
                        cached.seed.digest, cached.seed.permalink.empty() ? ", no permalink" : "") :
                    std::string("unreadable"));
        }
        if (cached.ok) {
            s_seeds.push_back(cached.seed);
        }
    }
    for (auto it = s_cache.begin(); it != s_cache.end();) {
        it = seen.count(it->first) != 0 ? std::next(it) : s_cache.erase(it);
    }
    std::sort(s_seeds.begin(), s_seeds.end(),
        [](const Seed& a, const Seed& b) { return a.hash < b.hash; });
    return s_seeds;
}

bool inventoryDependent(int item) {
    return std::find(std::begin(kInventoryDependent), std::end(kInventoryDependent), item) !=
           std::end(kInventoryDependent);
}

std::string fnv64Hex(std::string_view text) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 0x100000001b3ull;
    }
    return fmt::format("{:016x}", hash);
}

}  // namespace twili::rando_seeds
