#pragma once

// Shared progress between teammates: flags, event bits, items, keys and the full save merge.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>

namespace twili {

struct Client;

namespace sync {

// Connected, the room shares world state and the member gate lets us sync.
bool enabled();

// Save layout we announce and require from teammates (a test may force another one).
const std::string& localLayout();
void setLayoutOverrideForTest(const std::string& layout);

// Adds the team, protocol and layout fields every team-scoped packet carries.
void stampPacket(nlohmann::json& packet, bool addToQueue);

// Swap save data with the team as soon as a stage's save table is live.
void requestExchange();
void onStageSaveTableLoaded();
void onStageSaveTableUnloaded();

// True when the packet type belongs to world sync (handled or dropped here).
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Session::update while connected.
void tick();
// Session::update, last thing: our queued item grants may dispatch after mod_update.
void endOfUpdate();
void resetSession();
void shutdown();

// Hook entry points.
void onSetFlag(const char* category, int flagNo, int roomNo);
void onUnsetFlag(int flagNo, int roomNo);
void onEventBit(uint16_t no, bool set);
void onSaveWritten();
// execItemGet pre/post and the frame's actor pass (closes the grant dispatch window).
void beforeItemGet(uint8_t itemNo);
void afterItemGet(uint8_t itemNo);
void closeGrantWindow();
bool installItemObserver();
void removeItemObserver();

// GIVE_ITEM's filter: progression items, no consumables or dungeon items.
bool isSyncedItemNo(int itemNo);
// The shields that burn (Ordon and Wooden); personal unless the room shares them.
bool isWoodenShield(int itemNo);

// Story sync: our post-load world state went out (or none is due).
bool ownStateSettled();
// World-state merges applied from this client.
uint32_t mergeCount(uint32_t clientId);

// Autotest readback.
struct Stats {
    uint32_t merges = 0;
    uint32_t layoutRefusals = 0;
    uint32_t layoutWarnings = 0;
    uint32_t grantsQueued = 0;
    uint32_t grantsApplied = 0;
};
const Stats& stats();
// True once the incompatible-layout toast was shown for this teammate name.
bool layoutWarned(const std::string& name);

}  // namespace sync
}  // namespace twili
