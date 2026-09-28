#pragma once

// The server's view of every team: owner, team game and who may sync (TEAM_STATE).

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace twili::team_game {

enum class Sync : uint8_t { Pending, Ok, Unverified, Mismatch };
const char* syncName(Sync sync);

struct Game {
    bool set = false;
    std::string kind;
    // Own team only; other teams see the mode and seed name.
    std::string key;
    std::string name;
    std::string mode;
    std::string permalink;
    std::string seed;
    std::string version;
};

struct Member {
    uint32_t clientId = 0;
    Sync sync = Sync::Pending;
    bool inGame = false;
    std::string kind;
    std::string name;
    std::string mode;
};

struct Team {
    std::string id;
    uint32_t ownerClientId = 0;
    // Named teams only: what their members show in.
    bool hasColor = false;
    uint8_t colorR = 255, colorG = 255, colorB = 255;
    Game game;
    bool sameGameAsYours = false;
    std::vector<Member> members;
};

// TEAM_STATE and TEAM_GAME_CONFLICT.
bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Session::update while connected.
void tick();
void resetSession();

const std::map<std::string, Team>& teams();
const Team* ownTeam();
const Member* member(uint32_t clientId);
Sync localSync();
// The world-sync gate: our own state, or a teammate's as far as the server told us.
bool memberMaySync(uint32_t clientId);
bool isTeamOwner();

// The player confirmed an unverified randomizer match (this session), or always allows them.
bool unverifiedAllowed();
void allowUnverified();
// Team leader: make our game the team's game although teammates still play the old one.
void claimTeamGame();
bool conflictOpen();
// Hand the room (room owner) or our team (team leader) to another member.
void promoteRoomOwner(uint32_t clientId);
void promoteTeamLeader(uint32_t clientId);
// Team leader of a named team: the team's colour (sent at most four times a second).
bool canSetTeamColor();
void setTeamColor(uint8_t r, uint8_t g, uint8_t b);
// Our team's colour as last set here or reported; false without one.
bool teamColor(uint8_t& r, uint8_t& g, uint8_t& b);

// Why teleporting to this client is refused ("other-team", "other-game"), or null.
const char* teleportBlock(uint32_t clientId);

// UI text.
std::string gameLabel(const Game& game);
std::string memberBadge(const Member& member);
std::string statusRml();
std::string permalinkRml();
const std::string& ownPermalink();
bool copyPermalink();
std::string panelLine();

}  // namespace twili::team_game
