#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace twili::ui::color {

struct Rgb8 {
    uint8_t r = 255, g = 255, b = 255;
    bool operator==(const Rgb8&) const = default;
};

// h in degrees [0, 360), s and v in [0, 1]; every 8-bit colour survives the round trip.
struct Hsv {
    double h = 0.0, s = 0.0, v = 1.0;
};

double wrapHue(double h) noexcept;
Rgb8 hsvToRgb(const Hsv& c) noexcept;
// Greys have no hue and black has neither hue nor saturation: those come from `keep`.
Hsv rgbToHsv(Rgb8 c, const Hsv& keep) noexcept;
// "#RRGGBB", "RRGGBB", "#RGB" or "RGB", surrounding spaces ignored.
std::optional<Rgb8> parseHex(std::string_view text) noexcept;
std::string formatHex(Rgb8 c);  // "#RRGGBB"
// The config value: "rrggbb".
std::string formatConfig(Rgb8 c);
// WCAG contrast ratio against the name-tag fill (10, 12, 16).
double contrastOnTagFill(Rgb8 c) noexcept;
// Lightened until it reads on the tag fill with `minContrast`.
Rgb8 readableOnTagFill(Rgb8 c, double minContrast);

// Checks the maths over every 8-bit colour, `chunk` per call from `*next` (1 << 24 when done).
// Empty while all is well, else the first counterexample.
std::string selfTest(uint32_t* next, uint32_t chunk);

}  // namespace twili::ui::color
