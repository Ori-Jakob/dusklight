#pragma once

#include <cstdint>

// Whether a save is loaded; nothing syncs before that.
namespace twili {

void markSaveLoaded();
void clearSaveLoaded();
bool isSaveLoaded();
// Counts loads: a change means another save (or a reload) is in memory now.
uint32_t saveGeneration();

}  // namespace twili
