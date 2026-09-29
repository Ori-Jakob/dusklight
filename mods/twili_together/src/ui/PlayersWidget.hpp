#pragma once

#include "teleport/Teleport.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace twili {
struct Client;
}

// The players list on the overlay (UiService overlay panel): our team, or the room without one.
namespace twili::ui {

// A friendly area name for where the client is, or its stage id.
std::string areaText(const Client& client);

namespace players_widget {

// Once per tick; creates the panel once the host has an overlay.
void tick();
void shutdown();

struct Row {
    uint32_t clientId = 0;
    std::string name;
    std::string area;
    teleport::Offer offer = teleport::Offer::None;
    std::string tooltip;
};

// What the last tick showed (autotest).
struct Probe {
    bool supported = true;
    bool created = false;
    bool shown = false;
    // Why it is hidden: "off", "notConnected", "noSave", "empty", "cutscene", "menu", "" shown.
    const char* gate = "";
    int x = 0;
    int y = 0;
    int opacity = 0;
    std::string header;
    std::vector<Row> rows;
};
const Probe& probe();

// The host's click and drag callbacks, for the autotest.
void clickForTest(const std::string& elementId);
void moveForTest(float x, float y);

}  // namespace players_widget
}  // namespace twili::ui
