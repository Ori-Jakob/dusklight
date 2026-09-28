#pragma once

// Read-only view of the randomizer's generated seeds (mod_data/dev.twilitrealm.randomizer/seeds).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace twili::rando_seeds {

struct Seed {
    // Folder name: the seed's display name ("Soldier Beth Dragonfly").
    std::string hash;
    uint32_t formatVersion = 0;
    // mItemLocations: ItemService check name -> item, the checks the randomizer answers by name.
    std::vector<std::pair<std::string, int>> locations;
    // Over the placement data, independent of the file's key order.
    std::string digest;
    // From "<hash> Anti-Spoiler Log.txt"; empty when it is missing.
    std::string permalink;
    std::string seedString;
    std::string version;
};

// False when the text is not a usable seed.dat.
bool parseSeedData(std::string_view text, Seed& out);
void parseAntiSpoilerLog(std::string_view text, Seed& out);

// Every seed under `seedsDir`; files are parsed again only when they change.
const std::vector<Seed>& scan(const std::filesystem::path& seedsDir);

// Items the randomizer resolves against the current inventory (progressive and ammo items).
bool inventoryDependent(int item);

std::string fnv64Hex(std::string_view text);

}  // namespace twili::rando_seeds
