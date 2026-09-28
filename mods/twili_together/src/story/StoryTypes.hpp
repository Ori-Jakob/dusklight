#pragma once

// A story move is a stage change requested while a story event ran. Teammates elsewhere may
// follow: the destination entrance loads with their own synced flags, so the game builds the
// state a reload there would. Story segments say, from synced flags alone, where a player may be.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace twili::story {

using Clock = std::chrono::steady_clock;

// Stage + room + PLYR start point.
struct Entrance {
    char stage[8] = {};
    int8_t room = -1;
    int16_t point = -1;    // a real PLYR point; follow never uses -1
    int8_t layerArg = -1;  // what the originator's request passed (-1 = from flags)
    int8_t layer = -1;     // what the originator loaded, -1 unknown

    bool valid() const { return stage[0] != '\0' && room >= 0 && point >= 0; }
    void setStage(const char* name);
    bool sameStageRoom(const char* otherStage, int otherRoom) const;
    nlohmann::json toJson() const;
    static Entrance fromJson(const nlohmann::json& j);
};

// searchMapEventData looks in the room first.
enum class EventTable : uint8_t { None, Room, Stage };

// The event that ran when a stage change was requested.
struct EventRef {
    std::string name;
    int16_t eventId = -1;
    uint8_t mapToolId = 0xFF;
    uint8_t listType = 0;    // mEventId >> 8
    uint8_t mapType = 0xFF;  // 0xFF without map data
    uint8_t switchNo = 0xFF;
    int16_t requester = -1;  // profile name, -1 for none
    uint8_t mode = 0;        // dEvt_mode_*
    bool arrivalDemo = false;

    nlohmann::json toJson() const;
    static EventRef fromJson(const nlohmann::json& j);
};

enum class Form : uint8_t { Any, Human, Wolf };
const char* formName(Form form);

// Why a move counts as story.
enum Qual : uint32_t {
    kQualCurated = 1 << 0,
    kQualSidePoint = 1 << 1,  // the destination entrance has a d_s_play phase_1 side effect
    kQualForm = 1 << 2,
    kQualLevels = 1 << 3,     // transform or twilight-clear level changed
    kQualOneShot = 1 << 4,    // the map event has a switch
    kQualStoryBits = 1 << 5,  // this client set a synced event bit during the event
};
constexpr uint32_t kQualStrong = kQualCurated | kQualSidePoint | kQualForm | kQualLevels;

// One story move, as recorded by its originator or received in STORY_MOVE.
struct MoveRecord {
    std::string id;
    std::string originName;
    uint32_t originClientId = 0;
    Entrance from, to;
    EventRef event;
    bool wolfBefore = false, wolfAfter = false;
    uint8_t tlvBefore = 0, tlvAfter = 0, dclBefore = 0, dclAfter = 0;
    uint8_t arrivalEvent = 0xFF;  // daAlink_c::mStartEventID after arrival
    std::string arrivalName;
    int curated = -1;  // index into kStoryMoves
    uint32_t qual = 0;
    int hops = 1;
    // Local clock; minus the server's ageMs if cached.
    Clock::time_point at{};

    bool strong() const { return (qual & kQualStrong) != 0; }
    nlohmann::json toJson() const;
    static MoveRecord fromJson(const nlohmann::json& j);
};

// A curated story move: a label, a prompt text and a follower recipe.
struct StoryMoveDef {
    const char* id;
    const char* prompt;  // "{name} was captured"
    const char* place;
    // nullptr / -1 = any.
    const char* fromStage;
    int8_t fromRoom;
    const char* toStage;
    int8_t toRoom;
    const char* eventPrefix;
    // -1: the originator's point.
    int16_t followPoint;
    Form form;
    // Honour an explicit layer arg while the move is fresh.
    bool keepExplicitLayer;
    bool verified;
};

extern const StoryMoveDef kStoryMoves[];
extern const int kStoryMoveCount;

int matchCuratedMove(const MoveRecord& m);
const StoryMoveDef* curatedMove(int index);
// The entrance loads with a phase_1 side effect (twilight clear, transform level, horse flute).
bool isSideEffectPoint(const char* stage, int room, int point);
// kQual* bits of an arrived move; `localBitsDuring` synced event bits we set meanwhile.
uint32_t qualify(const MoveRecord& m, uint32_t localBitsDuring);
bool isNotStory(const std::string& eventName);
std::string placeName(const MoveRecord& m);

// A stretch of the story whose places cannot be reached from anywhere else.
struct StorySegment {
    const char* id;
    const char* text;
    bool (*active)();
    const char* const* allowedStages;  // nullptr-terminated; nullptr = anywhere
    Entrance canonical;                // stage[0] == 0: none known
    Form form;
    const char* place;
};

// What "Catch up to story" would do now.
struct CatchUpPlan {
    enum class Kind : uint8_t { None, Entrance, TeleportToPlayer };
    Kind kind = Kind::None;
    Entrance entrance;
    int8_t layerArg = -1;
    Form form = Form::Any;
    uint32_t clientId = 0;
    std::string moveId;   // "" for a segment repair
    std::string segment;  // "" if none
    bool inconsistent = false;
    std::string title;
    std::string reason;
    std::string place;
};
const char* catchUpKindName(CatchUpPlan::Kind kind);

enum class LoadPhase : uint8_t { Idle, Waiting, Loading, Arrived, Failed };
const char* loadPhaseName(LoadPhase phase);

enum class PromptKind : uint8_t { None, Move, Inconsistent };
const char* promptKindName(PromptKind kind);
enum class PromptAnswer : uint8_t { Follow, Decline, CatchUp };

// Same-room pull-in.
enum class JoinState : uint8_t { None, Waiting, Ordered, Running, Missed, Shared, Ended };
const char* joinStateName(JoinState state);

const StorySegment* activeSegment();
const StorySegment* segmentById(const std::string& id);
bool segmentAllows(const StorySegment& seg, const char* stage);
// dComIfGs_Wolf_Change_Check for a spawn at `e`, with the entrance's own phase_1 levels applied.
bool predictSpawnWolf(const Entrance& e);

}  // namespace twili::story
