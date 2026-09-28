#pragma once

// Whether a save is loaded; nothing syncs before that.
namespace twili {

void markSaveLoaded();
void clearSaveLoaded();
bool isSaveLoaded();

}  // namespace twili
