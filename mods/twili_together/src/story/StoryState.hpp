#pragma once

// Internal to story/*.cpp and the story autotest steps.

#include "story/Story.hpp"
#include "story/StoryTracker.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>

namespace twili::story::detail {

// Version carried in STORY_EVENT and STORY_MOVE ("sv").
inline constexpr int kSyncVersion = 1;

struct TeamMove {
    MoveRecord move;
    bool valid = false;
    // Followed, or we stand in its destination.
    bool satisfied = false;
    bool declined = false;
    // Its pop-up or toast went out.
    bool offered = false;
    bool fromCache = false;
    // Its originator is still on a cutscene or battle layer we would not load.
    bool transient = false;
    bool transientExpired = false;
    Clock::time_point transientSince{};
    // Received, or no longer transient: a newer move of the same chain may still follow.
    Clock::time_point offerableAt{};
};

struct LoadState {
    LoadPhase phase = LoadPhase::Idle;
    Entrance entrance;
    int8_t layerArg = -1;
    Form form = Form::Any;
    std::string moveId;
    std::string source;
    bool needSync = true;
    bool sawUnload = false;
    bool leftOld = false;
    bool formChecked = false;
    bool chainOpen = false;
    // The originator whose world state the form check waits for.
    uint32_t waitMergeFrom = 0;
    uint32_t mergeBaseline = 0;
    Clock::time_point requestedAt{}, loadStartedAt{}, mergeWaitUntil{};
    std::string reason;
    std::string message;
};

struct PromptState {
    PromptKind kind = PromptKind::None;
    bool showing = false;
    // Offered, waiting for the player to be free.
    bool waitingCalm = false;
    Clock::time_point waitingSince{};
    std::string moveId;
    int inconsistentDeclines = 0;
    // Stage load whose consistency check already ran.
    uint32_t checkedLoadSeq = 0;
};

struct JoinInfo {
    JoinState state = JoinState::None;
    uint32_t originClientId = 0;
    uint32_t originInstance = 0;
    std::string originName;
    std::string eventName;
    uint8_t mapToolId = 0xFF;
    bool autoNext = false;
    // The retry re-opens the event's switch around the order.
    bool bypassSwitch = false;
    uint8_t switchNo = 0xFF;
    // Waiting / Ordered since.
    uint32_t startTick = 0;
    int retries = 0;
    uint32_t localInstance = 0;
    // An NPC's event: our copy of that NPC orders its table entry npcIndex.
    bool npc = false;
    int16_t npcIndex = 0;
    uint32_t npcId = 0;
    bool originEnded = false;
    Clock::time_point runningSince{}, originEndedAt{};
    std::string reason;
};

struct ClientMove {
    std::string text;
    Clock::time_point at{};
};

struct State {
    TeamMove team;
    LoadState load;
    PromptState prompt;
    CatchUpPlan plan;
    JoinInfo join;
    std::map<uint32_t, ClientMove> clientMoves;
    // Moves already followed or declined; kept across sessions, dropped with the save.
    std::set<std::string> answeredMoves;
    // Our announced instance and the teammates that joined it.
    uint32_t announcedInstance = 0;
    std::set<uint32_t> joinedBy;
    bool hadSave = false;
    std::string forcedBlocker;
    uint32_t movesSent = 0;
    uint32_t movesReceived = 0;
    std::string lastMoveCurated;
    uint32_t lastMoveQual = 0;
    bool lastMoveFromCache = false;
    MoveRecord lastMove;
    // Segments whose predicate our own side undid (the randomizer's return to spawn clears
    // levels and story bits that world sync merges back): menu row only, no pop-up.
    std::set<std::string> leftSegments;
    std::string lastSegment;
    uint8_t lastTlv = 0, lastDcl = 0;
};

State& state();

// StoryMove.cpp
// "just now", "3 min ago".
std::string ago(Clock::time_point t);
// "Kira was captured".
std::string moveTitle(const MoveRecord& m);
void sendPacket(nlohmann::json packet);
// Team, sync, save, layout and version checks shared by both story packets.
bool acceptsPacket(const nlohmann::json& packet, bool needCutsceneSync);
void onOwnMoveSettled(MoveRecord move, uint32_t localBitsDuring, uint32_t copyOf);
void handleStoryMove(const nlohmann::json& packet);
// A follow arrived and its destination's arrival events still play.
bool followChainOpen();
void computeCatchUpPlan();
const char* catchUpBlockCode();
bool startCatchUpPlan();
void answerPrompt(PromptAnswer answer);
void closePrompt();
void tickLoad();
void tickPrompt();
std::string debugText();

// StoryEvent.cpp
void onInstanceAccepted(const Instance& in);
// The originator's instance id if `order` is the pull-in copy we just placed, else 0.
uint32_t joinClaims(const dEvt_order_c& order);
void handleStoryEvent(const nlohmann::json& packet);
void tickJoin();
const char* pullInBlocker();

}  // namespace twili::story::detail
