#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk {

using CommandOutput = std::function<void(std::string)>;

struct CommandState {
    std::vector<std::string> history;
    unsigned int foundProcId = 0;
    std::string lastWarpStage;
    bool echoEnabled = true;
    std::string tracePath;
    unsigned int traceFrames = 0;
    bool replaySource = false;
};

// Execute a single command line. Calls output() for every line of response.
// Manages history internally; callers need only hold a CommandState.
void runCommand(std::string_view cmdLine, CommandState& state, const CommandOutput& output);

// Per-sim-tick update for camera fly animation.
void processCameraCommands();

// Returns true when the dev camera is detached (suppresses the game's camera update).
bool isCameraDetached();

// Called every frame so a trace capture started from this state can report how it ended.
void pollCommands(CommandState& state, const CommandOutput& output);

bool isRenderTraceActive();

}  // namespace dusk
