#pragma once

#include <mods/api.h>
#include <mods/svc/ui.h>

#include <cstdint>
#include <string>
#include <vector>

// The "Twili-Together" menu tab, its window (Connection, Room, Players) and the Mods panel.
namespace twili::ui {

ModResult registerWindow();
void shutdownWindow();

bool openWindow();
void closeWindow();
bool windowOpen();
// The Connection tab's colour control while that tab is shown, else 0.
UiElementHandle colorControl();

// The Players tab's rows while that tab is shown (autotest).
struct PlayersTabRow {
    uint32_t clientId = 0;
    bool teleport = false;
    bool blocked = false;
    std::string tooltip;
    bool actions = false;
};
std::vector<PlayersTabRow> playersTabRows();
bool playersTabShown();

}  // namespace twili::ui
