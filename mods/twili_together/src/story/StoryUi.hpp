#pragma once

#include <cstdint>
#include <string>

// What the Players tab shows about story sync; P5 replaces the stub.
namespace twili::story {

// Status lines for the Story section, RML.
std::string statusRml();
// A teammate's last story move ("captured, 2 min ago"), "" if none. Plain text.
std::string clientLine(uint32_t clientId);
// The "Catch up to story" button text without its reason. Plain text.
std::string catchUpTitle();
// nullptr if catching up may run now, else a reason code (teleport::reasonText).
const char* catchUpBlockCode();
// The confirmation's body: where, why, in which form. RML.
std::string catchUpConfirmRml();
// The confirmation was accepted; re-checks everything.
void startCatchUp();

}  // namespace twili::story
