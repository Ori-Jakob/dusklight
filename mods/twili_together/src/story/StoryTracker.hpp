#pragma once

// Recognises story events by their effect: a stage change requested while an event runs.

#include "core/SpawnKey.hpp"
#include "story/StoryTypes.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

class dEvt_order_c;

namespace twili::story {

// copyOf of a relocation that continues our own follow load.
constexpr uint32_t kFollowChainCopy = 0xFFFFFFFFu;

// The game's tick counter (once per simulated frame).
uint32_t nowTick();

// One accepted event order.
struct Instance {
    uint32_t id = 0;   // continuations keep the id of the event they continue
    uint32_t seq = 0;  // per accepted order
    int16_t eventId = -1;
    uint8_t listType = 0;
    uint8_t mapToolId = 0xFF;
    uint8_t mapType = 0xFF;
    uint8_t switchNo = 0xFF;
    uint8_t next = 0xFF;      // the auto-next successor
    uint8_t exit = 0xFF;      // the SCLS exit taken when it ends
    uint8_t skipExit = 0xFF;  // the exit taken when skipped
    EventTable table = EventTable::None;
    uint16_t orderType = 0;
    uint16_t flag = 0;
    int16_t requester = -1;
    bool requesterIsPlayer = false;
    ReqKind reqKind = ReqKind::None;
    SpawnKey reqKey;  // actor requesters only
    uint8_t tagEventNo = 0xFF;  // daTag_Event_c requester: its event no and switch
    uint8_t tagSwbit = 0xFF;
    bool arrivalDemo = false;
    bool continuation = false;
    uint32_t copyOf = 0;  // a pull-in copy of the originator's instance
    uint8_t mode = 0;
    uint16_t eventFlag = 0;
    char stage[8] = {};
    int8_t room = -1;
    int8_t layer = -1;
    bool wolf = false;
    uint32_t bitsAtAccept = 0;
    std::string name;
    uint32_t acceptedTick = 0;
    uint32_t endedTick = 0;  // 0 while it runs
    bool running() const { return endedTick == 0; }
};

class Tracker {
public:
    void onEventAccepted(const dEvt_order_c& order);
    void onStageSaveTableLoaded();
    void noteLocalEventBit(uint16_t no);
    uint32_t localEventBitsSet() const { return mLocalEventBitsSet; }

    void tick();

    // The event running now or ended less than kContinueTicks ago.
    const Instance* openInstance() const;
    const Instance* lastInstance() const {
        return mInstances.empty() ? nullptr : &mInstances.back();
    }
    uint32_t stageLoadSeq() const { return mStageLoadSeq; }
    // Game ticks since Link started executing in the current stage, 0 while loading.
    uint32_t ticksSinceLoad() const;
    // Game ticks since the last event ended (or the stage loaded), 0 while one runs.
    uint32_t quietTicks() const;
    bool movePending() const { return mMove.has_value(); }
    std::optional<Clock::time_point> lastOwnStoryArrival() const { return mLastOwnStoryArrival; }
    void setLastOwnStoryArrival(Clock::time_point t) { mLastOwnStoryArrival = t; }

    std::string describe() const;
    void clear();

private:
    struct PendingMove {
        MoveRecord move;
        uint32_t bitsAtAccept = 0;
        // The bit counter when the departure stage loaded.
        uint32_t bitsAtVisit = 0;
        uint32_t copyOf = 0;
        bool sawUnload = false;
        bool leftOld = false;
        bool arrived = false;
        uint32_t lastTick = 0;
        uint32_t quiet = 0;
        Clock::time_point departedAt{}, arrivedAt{};
    };

    struct BitNote {
        uint32_t index = 0;  // mLocalEventBitsSet after it
        uint16_t no = 0;
        bool story = false;  // set while an event ran or a move was under way
    };

    bool captureDeparture(PendingMove& out);
    void recordArrival();
    void settle();
    // The move's bits so far; returns how many story bits since the departure stage loaded.
    uint32_t collectBits(PendingMove& p) const;

    std::deque<Instance> mInstances;
    uint32_t mNextInstanceId = 1;
    uint32_t mNextSeq = 1;
    uint32_t mLocalEventBitsSet = 0;
    uint32_t mBitsAtStageLoad = 0;
    std::deque<BitNote> mBitLog;
    bool mSawEventSinceLoad = false;
    bool mHadSave = false;
    bool mPrevPending = false;
    bool mLinkWasLive = false;
    uint32_t mStageLoadSeq = 0;
    uint32_t mStageLiveTick = 0;
    uint32_t mLastEventEndTick = 0;
    bool mEventWasRunning = false;
    std::optional<PendingMove> mMove;
    std::optional<Clock::time_point> mLastOwnStoryArrival;
};

Tracker& tracker();

// Logs the stay room's and the stage's map events, exits, PLYR points and event tags.
void dumpStageEvents();

uint8_t transformLevels();
uint8_t darkClearLevels();

}  // namespace twili::story
