#include "core/Visibility.hpp"

#include "core/Session.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_event.h"

#include <chrono>

namespace twili {

namespace {

constexpr auto kCutsceneReleaseDelay = std::chrono::milliseconds(350);
// dEvt_control_c::doorCheck sets this for door demos (chests set 4 instead).
constexpr uint16_t kEventFlagDoor = 0x40;

int s_cutsceneOverride = -1;
std::chrono::steady_clock::time_point s_cutsceneReleaseAt{};

}  // namespace

bool isCutsceneEvent(bool eventRunning, uint8_t eventMode, uint16_t eventFlags) {
    return eventRunning && eventMode == dEvt_mode_DEMO_e && (eventFlags & kEventFlagDoor) == 0;
}

bool localCutsceneRunning() {
    const bool running =
        s_cutsceneOverride >= 0 ?
            s_cutsceneOverride != 0 :
            isCutsceneEvent(dComIfGp_event_runCheck() != FALSE, dComIfGp_event_getMode(),
                            dComIfGp_event_chkEventFlag(0xFFFF));
    const auto now = std::chrono::steady_clock::now();
    if (running) {
        s_cutsceneReleaseAt = now + kCutsceneReleaseDelay;
        return true;
    }
    return now < s_cutsceneReleaseAt;
}

bool hideRemotePlayersForCutscene() {
    return Session::instance().roomState().hidePlayersInCutscene && localCutsceneRunning();
}

bool hideRemoteClientForCutscene(const Client& client) {
    if (!client.hasPlayerUpdate) {
        return false;
    }
    // A shared story cutscene shows each game's own Link.
    if (Session::instance().storySharedEventWith(client.clientId)) {
        return true;
    }
    return Session::instance().roomState().hidePlayersInCutscene &&
           (client.presenceFlags & kPresenceInCutscene) != 0;
}

void setCutsceneOverrideForTest(int value) {
    s_cutsceneOverride = value;
}

}  // namespace twili

