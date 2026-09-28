#pragma once

#include <cstdint>
#include <string>

// Item names from the game's own message archive, English as the fallback.
namespace twili::ui {

// UTF-8, never empty. `saveTbl` picks the stage-specific names (the Snowpeak Ruins map).
const std::string& itemDisplayName(uint8_t itemNo, int saveTbl = -1);
bool itemNameFromGame(uint8_t itemNo, int saveTbl = -1);
// "Forest Temple" for dStage_SaveTbl_LV1, "" for tables that are not a dungeon.
const char* dungeonNameForSaveTbl(int saveTbl);

}  // namespace twili::ui
