#pragma once

#include <cstdint>
#include <string>

// Item names from the game's own message archive, English as the fallback.
namespace twili::ui {

// Game: the disc's message; Mod: our name for a randomizer item or one the game names poorly;
// Fallback: our English table (discs without a usable name); Unknown: an id nothing uses.
enum class ItemNameSource : uint8_t { Game, Mod, Fallback, Unknown };

// UTF-8, never empty. `saveTbl` picks the stage-specific names (the Snowpeak Ruins map).
const std::string& itemDisplayName(uint8_t itemNo, int saveTbl = -1);
ItemNameSource itemNameSource(uint8_t itemNo, int saveTbl = -1);
// "Forest Temple" for dStage_SaveTbl_LV1, "" for tables that are not a dungeon.
const char* dungeonNameForSaveTbl(int saveTbl);

}  // namespace twili::ui
