#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Name tags over remote players and cards over their unridden horses, drawn under the HUD from a
// GfxService stage callback once per presented frame.
namespace twili::ui::name_tags {

bool install();
void uninstall();

// What the last frame drew (autotest). Text in the font's bytes; x, y the tag's box corner.
struct Probe {
    enum class Kind : uint8_t { Player, HorseCard };
    uint32_t clientId = 0;
    Kind kind = Kind::Player;
    std::string line1;
    std::string line2;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};
const std::vector<Probe>& drawn();
// Frames the callback ran, and the last one's reason for drawing nothing ("" when it drew).
uint32_t frameCount();
const char* lastGate();

// The card over `clientId`'s horse, "<owner>'s <horse>", "" while none shows; the second line of
// its tag, the horse's name while it rides her.
std::string horseCardText(uint32_t clientId);
std::string riderTagLine2(uint32_t clientId);

}  // namespace twili::ui::name_tags
