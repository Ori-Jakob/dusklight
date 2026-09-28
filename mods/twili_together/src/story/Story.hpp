#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>

class dEvt_order_c;

// Story sync: STORY_MOVE follow prompts, catch-up and repair, and the same-room pull-in
// (STORY_EVENT). The tracker runs whether or not we are connected.
namespace twili::story {

// dEvt_control_c::setParam: an event order was accepted.
void onEventAccepted(const dEvt_order_c& order);
// dSv_info_c::getSave: a stage is loading.
void onStageSaveTableLoaded();
// World sync wrote one of our synced event bits.
void noteLocalEventBit();

bool handlePacket(const std::string& type, const nlohmann::json& packet);
// Every Session::update, after teleport::tick, connected or not.
void tick();
void resetSession();
void shutdown();

// A follow or catch-up load is waiting or under way.
bool loadActive();
// We run a copy of this client's cutscene, or it runs ours: its dummy hides.
bool sharedEventWith(uint32_t clientId);

}  // namespace twili::story
