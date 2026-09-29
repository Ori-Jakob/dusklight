#pragma once

// Scripted self-test driven by the autotest_script cvar; see tools/autotest/README.md.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

struct PADStatus;
class scene_class;

namespace twili::autotest {

// Reads the script, if set. False if it is unusable (reported at the first tick).
bool init();
bool isActive();

// Once per simulation tick.
void tick();

// True if the boot was taken over (skip the original).
bool takeOverBoot(scene_class* logoScene);
// The story fields of a script's start (StepsStory.cpp), before the first load.
void applyStoryStart(const nlohmann::json& start);
// A stage's save table for those fields, -1 if unknown.
int stageSaveTbl(const char* stage);
// The testRemap step's stand-in for the randomizer's entrance shuffle (dComIfGp_setNextStage).
void remapStageRequest(const char*& stage, int16_t& point, int8_t& room, int8_t& layer);

// Scripted input on port 0, neutral otherwise.
void overridePad(PADStatus* status);

// Synthetic input for the next `ticks` ticks, e.g. to answer a menu.
void pulsePad(float stickX, float stickY, uint16_t buttons, int ticks);
bool padBusy();

uint64_t ticks();

// Session operations behind the network steps; installed by the session.
class NetDriver {
public:
    virtual ~NetDriver() = default;
    virtual void connect(const std::string& url, const std::string& name, const std::string& room,
        const std::string& team) = 0;
    virtual void disconnect() = 0;
    // Joined a room with a client id.
    virtual bool connected() const = 0;
    virtual uint32_t selfClientId() const = 0;
    // Non-empty once a connection attempt failed for good.
    virtual std::string failureMessage() const = 0;
    virtual int countPeers(bool sameStage) const = 0;
    virtual int countDummies() const = 0;
    virtual bool checkDummies(float maxDist, std::string& why) const = 0;
    virtual void sendSignal(const std::string& instance, const std::string& name) = 0;
};

void setNetDriver(NetDriver* driver);
// The session's driver (NetDriver.cpp).
void installNetDriver();
void removeNetDriver();

// An AUTOTEST_SIGNAL relayed by the server.
void onSignal(const std::string& instance, const std::string& name);

}  // namespace twili::autotest
