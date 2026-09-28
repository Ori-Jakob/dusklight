#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// Teleport to another player (REQUEST_TELEPORT / TELEPORT_TO), as the Players tab uses it.
namespace twili::teleport {

enum class Phase : uint8_t {
    Idle,
    AwaitingAnswer,
    Executing,
    AwaitingArrival,
};

enum class Result : uint8_t { None, MovedLocally, ChangedStage, Refused, TimedOut, Failed };

struct Status {
    Phase phase = Phase::Idle;
    Result lastResult = Result::None;
    uint32_t targetClientId = 0;
    std::string reason;
    std::string message;
    // Bumped whenever lastResult is written.
    uint32_t resultSeq = 0;
};

// Why teleporting to `clientId` is refused now, or nullptr.
const char* blockCode(uint32_t clientId);
// Text for a reason code, also used for story codes ("in a cutscene").
std::string reasonText(std::string_view code);
// Re-checks everything; a refusal ends up in status() too.
bool request(uint32_t clientId);
// The last request's status line, "" before the first.
const std::string& statusMessage();
const Status& status();
bool idle();
// "none", "local", "stage", "refused", "timeout", "failed".
const char* resultName(Result result);
// The roster name, or "#<id>".
std::string clientName(uint32_t clientId);

bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Every Session::update, connected or not: a stage change under way is seen through.
void tick();

}  // namespace twili::teleport
