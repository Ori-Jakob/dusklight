#pragma once

// What this game plays, announced to the server so world sync stays within one game.

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace twili::game_identity {

struct Identity {
    bool inGame = false;
    // "vanilla", "randomizer" or "mode" (another mod's game mode).
    std::string kind;
    // vanilla, rando/f<format>/<digest>, rando-probe/<fingerprint> or mode/<id>; empty out of game.
    std::string key;
    // The seed's name for a randomizer game.
    std::string name;
    std::string mode;
    // False when only item probes back the key (no seed file matched).
    bool verified = true;
    // From the seed's anti-spoiler log; shared with the own team only.
    std::string permalink;
    std::string seed;
    std::string version;
};

const Identity& current();
bool isUnverifiedKey(const std::string& key);

// The HANDSHAKE "game" field; what the server knows from then on.
nlohmann::json handshakeJson();
// Session::update while connected: detects changes and announces them.
void tick();
// Detect again on the next tick (a seed may have been loaded or created).
void invalidate();
// Announce again even if nothing changed (e.g. the unverified match was confirmed).
void republish();
void resetSession();

#if TWILI_ENABLE_AUTOTEST
// Replaces detection; a null or empty object restores it.
void setOverrideForTest(const nlohmann::json& identity);
#endif

}  // namespace twili::game_identity
