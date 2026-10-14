#pragma once

#include "dusk/commands.hpp"

#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dusk::input_replay {

enum class Mode : uint8_t {
    Idle,
    Recording,
    Replaying,
    Finished,
};

struct Options {
    std::string recordPath;
    std::string replayPath;
    std::string hashLogPath;
    bool exitOnReplayEnd = false;
};

struct TickInput {
    PADStatus pad[PAD_CHANMAX];
    bool resetSwitch;
    uint32_t rumbleSupported;
    uint32_t actionBits;
    float mouse[4];
    bool gyroReset;
    float gyro[3];
    float touch[2];
};

struct Position {
    Mode mode;
    std::string file;
    uint32_t tick;
    bool anchored;
};

bool configure(const Options& options);
void apply_setting_overrides();
void on_game_start();
void before_frame();
void begin_tick();
void end_tick();
void shutdown();
int exit_code();

Mode mode();
Position position();
TickInput* override_input();
TickInput* capture_input();

void filter_pad_status(PADStatus* status, bool& resetSwitch, uint32_t& rumbleSupported);
uint32_t filter_action_mask(uint32_t port, uint32_t mask);
uint8_t filter_loaded_save(uint8_t* cardBuffer, uint32_t slot, uint8_t status);
bool virtual_os_time(OSTime& out);

void dvd_command_queued();
void dvd_command_finished();
void dvd_gate_wait();
void wave_load_queued();
void wave_load_finished();

bool accept_console_command(
    std::string_view cmd, std::string_view line, bool fromReplay, const CommandOutput& output);
void print_status(const CommandOutput& output);
void print_hash(const CommandOutput& output);
void stop(const CommandOutput& output);

}  // namespace dusk::input_replay
