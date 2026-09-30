#pragma once

// The item model the local player holds up, as peers are to show it

#include "presence/RemotePose.hpp"

#include <cstdint>

class daAlink_c;

namespace twili::getitem {

inline constexpr int kWireSize = 7;

// The event's daDitem_c as it was drawn last tick (none while nothing is shown).
void captureLocal(daAlink_c* link, uint32_t seq, RemoteGetItem& out);
void encode(const RemoteGetItem& g, int32_t out[kWireSize]);
RemoteGetItem decode(const int32_t in[kWireSize]);

// Autotest
struct SenderStats {
    uint32_t shownTicks = 0;  // captures with an item shown
    uint32_t shows = 0;       // items shown after a capture without
    uint8_t lastItem = 0xFF;  // display item of the latest
    uint8_t lastRealItem = 0xFF;
};
const SenderStats& senderStats();

}  // namespace twili::getitem
