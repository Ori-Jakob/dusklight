#pragma once

// Internal to sync/*.cpp.

#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace twili::sync::detail {

using Clock = std::chrono::steady_clock;

// Memory switches and event bits cleared this session: merges never OR them back.
struct ClearedFlags {
    uint32_t switches[dSv_save_c::STAGE_MAX][4] = {};
    uint8_t eventBits[MAX_EVENTS] = {};
};

struct State {
    // Per session.
    bool exchangePending = false;
    bool catchUpRequested = false;
    Clock::time_point ownStatePublishAt{};
    ClearedFlags cleared;
    // Save tables whose key count the next merge may take from the snapshot.
    uint32_t keyCountAdoptable = 0;
    bool dungeonBaselineValid = false;
    int dungeonBaselineSaveTbl = -1;
    int dungeonBaselineKeyNum = 0;
    uint8_t dungeonBaselineItems = 0;
    std::set<uint32_t> versionMismatchLogged;
    std::set<uint32_t> layoutWarnedIds;
    std::map<uint32_t, uint32_t> merges;

    // Kept across reconnects, dropped with the save: the catch-up replays the whole queue.
    std::string queueEpoch;
    std::set<uint64_t> queueApplied;
    uint32_t saveGeneration = 0;

    // Our grants waiting in ItemService's queue, oldest first.
    std::deque<uint8_t> pendingGrants;
    bool grantWindow = false;

    std::set<std::string> layoutWarnedNames;
    std::string layoutOverride;
    Stats stats;
};

State& state();

int currentSaveTblNo();
// Sender known, same team and layout, member gate open, not a replay we already applied.
bool acceptsWorldPacket(const nlohmann::json& packet, const char* what);
void stampWorldPacket(nlohmann::json& packet, bool addToQueue);
void send(nlohmann::json packet);
// Our save changed since the last check: forget what applied to the old one.
void checkSaveGeneration();

// Flags.cpp
void handleSetFlag(const nlohmann::json& packet);
void handleUnsetFlag(const nlohmann::json& packet);
void handleSetEventBit(const nlohmann::json& packet);
void handleUnsetEventBit(const nlohmann::json& packet);
void noteMemorySwitch(int saveTblNo, int switchNo, bool set);
void noteEventBit(uint16_t no, bool set);

// WorldState.cpp
void sendRequestWorldState();
void sendUpdateWorldState(uint32_t targetClientId = 0);
void handleRequestWorldState(const nlohmann::json& packet);
void handleUpdateWorldState(const nlohmann::json& packet);

// LightDrops.cpp
void handleLightDrop(const nlohmann::json& packet);

// GiveItem.cpp
void sendGiveItem(uint8_t itemNo);
void handleGiveItem(const nlohmann::json& packet);

// DungeonItems.cpp
void tickDungeonItemTracking();
void shiftDungeonItemBaseline(int keyNumBefore, uint8_t dungeonItemsBefore);
void handleUpdateDungeonItems(const nlohmann::json& packet);

}  // namespace twili::sync::detail
