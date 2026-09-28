#pragma once

// Internal to the autotest; feature steps use AutoTestSteps.hpp.

#include <chrono>
#include <cstdint>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace twili::autotest::detail {

using Clock = std::chrono::steady_clock;

constexpr int kExitPass = 0;
constexpr int kExitFail = 2;
constexpr int kExitTimeout = 3;

struct State {
    bool active = false;
    bool bootTaken = false;
    bool finished = false;
    int exitCode = kExitPass;
    // Reported at the first tick.
    std::string initFailure;

    std::string scriptPath;
    std::string instance;
    std::string url;
    std::string room;
    std::string team;
    std::string resultPath;
    double timeoutSec = 240.0;
    nlohmann::json start;
    nlohmann::json steps;

    Clock::time_point startedAt;
    Clock::time_point stepStartedAt;
    size_t stepIndex = 0;
    bool stepBegun = false;
    // Game time: counted once per simulation tick, not per frame.
    uint64_t simTicks = 0;
    uint64_t stepStartTick = 0;
    bool stageLoaded = false;
    uint64_t stageLoadedTick = 0;

    bool padActive = false;
    int padFrame = 0;
    uint64_t padEndTick = 0;
    float padStickX = 0.0f;
    float padStickY = 0.0f;
    bool padCircle = false;
    uint16_t padButtons = 0;

    bool hasMark = false;
    float markX = 0.0f;
    float markY = 0.0f;
    float markZ = 0.0f;

    std::set<std::string> signals;
    int maxPeers = 0;
    int maxDummies = 0;
    int warps = 0;
};

State& state();

double secondsSince(Clock::time_point t);
void finish(bool pass, int code, const std::string& reason);
void fail(const std::string& reason);

void applyDebugStatus();
void suppressErrorDialogs();

}  // namespace twili::autotest::detail
