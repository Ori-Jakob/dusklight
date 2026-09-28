#pragma once

// The local player's Epona as peers are to show her

#include "presence/RemotePose.hpp"

#include <cstdint>
#include <string>

namespace twili {
struct Client;
struct HorsePlace;
}  // namespace twili

namespace twili::horse {

// A new session
void resetSender();
// Our horse this tick
bool captureLocal(RemoteHorsePose& out);

// All zero while the horse is not present, the rider group while nobody rides.
void encode(const RemoteHorsePose& h, WirePose& w);
RemoteHorsePose decode(const WirePose& w);

std::string localNameUtf8();
bool localPlace(HorsePlace& out);

// The name a card shows for `c`'s horse
const std::string& remoteName(const Client& c);

}  // namespace twili::horse
