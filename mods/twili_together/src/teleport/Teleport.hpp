#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Teleport to another player, as the Players tab uses it; P5 replaces the stub.
namespace twili::teleport {

// Why teleporting to `clientId` is refused now, or nullptr.
const char* blockCode(uint32_t clientId);
// Text for a reason code, also used for story codes ("in a cutscene").
std::string reasonText(std::string_view code);
// Re-checks everything; a refusal ends up in statusMessage().
void request(uint32_t clientId);
// The last request's status line, "" before the first.
const std::string& statusMessage();

}  // namespace twili::teleport
