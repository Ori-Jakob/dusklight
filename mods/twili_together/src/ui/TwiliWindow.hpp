#pragma once

#include <mods/api.h>
#include <mods/svc/ui.h>

// The "Twili-Together" menu tab, its window (Connection, Room, Players) and the Mods panel.
namespace twili::ui {

ModResult registerWindow();
void shutdownWindow();

bool openWindow();
void closeWindow();
bool windowOpen();
// The Connection tab's colour control while that tab is shown, else 0.
UiElementHandle colorControl();

}  // namespace twili::ui
