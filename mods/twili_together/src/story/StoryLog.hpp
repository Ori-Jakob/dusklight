#pragma once

// The team story log: recent moves in memory and, per game identity, the learned destinations.

#include "story/StoryTypes.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace twili::story::storylog {

struct Learned {
    MoveRecord move;  // the newest one seen with this key
    uint32_t seen = 0;
    int64_t lastSeenMs = 0;  // unix time
    bool own = false;        // our own move
};

// A settled move of ours or a teammate's. Only validated, qualifying moves are learned.
void note(const MoveRecord& m, bool own);
// Oldest first, deduplicated by key.
const std::deque<MoveRecord>& recent();
const Learned* learnedByKey(const std::string& key);
// The newest learned move with this curated id.
const Learned* learnedByCurated(const char* curatedId);
size_t learnedCount();
const std::map<std::string, Learned>& learned();
// Our own story arrival (a move or a follow); stored, so older learned moves stay done.
void noteOwnStoryArrival();
int64_t lastOwnStoryMs();
int64_t unixMs();
// Drops the memory log and the current game's learned file.
void forget();
// Session::update: follows the game identity and applies a finished file load.
void tick();
void shutdown();

}  // namespace twili::story::storylog
