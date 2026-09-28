#include "ui/ColorMath.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace twili::ui::color {
namespace {

double luminance(Rgb8 c) noexcept {
    const auto linear = [](uint8_t v) {
        const double x = v / 255.0;
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

}  // namespace

double wrapHue(double h) noexcept {
    h = std::fmod(h, 360.0);
    if (h < 0.0) {
        h += 360.0;
    }
    return h >= 360.0 ? 0.0 : h;
}

Rgb8 hsvToRgb(const Hsv& in) noexcept {
    const double s = std::clamp(in.s, 0.0, 1.0);
    const double v = std::clamp(in.v, 0.0, 1.0);
    const double h6 = wrapHue(in.h) / 60.0;
    const double sextant = std::floor(h6);
    const double f = h6 - sextant;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    double r, g, b;
    switch (static_cast<int>(sextant) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    const auto to8 = [](double x) { return static_cast<uint8_t>(std::lround(x * 255.0)); };
    return {to8(r), to8(g), to8(b)};
}

Hsv rgbToHsv(Rgb8 c, const Hsv& keep) noexcept {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double d = mx - mn;
    Hsv out{keep.h, keep.s, mx};
    if (mx <= 0.0) {
        return out;
    }
    out.s = d / mx;
    if (d <= 0.0) {
        return out;
    }
    const double h = mx == r ? (g - b) / d : mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0;
    out.h = wrapHue(h * 60.0);
    return out;
}

std::optional<Rgb8> parseHex(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6 && text.size() != 3) {
        return std::nullopt;
    }
    int digits[6] = {};
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        if (ch >= '0' && ch <= '9') {
            digits[i] = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digits[i] = ch - 'a' + 10;
        } else {
            return std::nullopt;
        }
    }
    if (text.size() == 3) {
        return Rgb8{static_cast<uint8_t>(digits[0] * 17), static_cast<uint8_t>(digits[1] * 17),
            static_cast<uint8_t>(digits[2] * 17)};
    }
    return Rgb8{static_cast<uint8_t>(digits[0] * 16 + digits[1]),
        static_cast<uint8_t>(digits[2] * 16 + digits[3]),
        static_cast<uint8_t>(digits[4] * 16 + digits[5])};
}

std::string formatHex(Rgb8 c) {
    return fmt::format("#{:02X}{:02X}{:02X}", c.r, c.g, c.b);
}

std::string formatConfig(Rgb8 c) {
    return fmt::format("{:02x}{:02x}{:02x}", c.r, c.g, c.b);
}

double contrastOnTagFill(Rgb8 c) noexcept {
    const double a = luminance(c);
    const double fill = luminance({10, 12, 16});
    return (std::max(a, fill) + 0.05) / (std::min(a, fill) + 0.05);
}

Rgb8 readableOnTagFill(Rgb8 c, double minContrast) {
    if (contrastOnTagFill(c) >= minContrast) {
        return c;
    }
    Hsv hsv = rgbToHsv(c, {0.0, 0.0, 1.0});
    while (hsv.v < 1.0) {
        hsv.v = std::min(1.0, hsv.v + 0.05);
        c = hsvToRgb(hsv);
        if (contrastOnTagFill(c) >= minContrast) {
            break;
        }
    }
    return c;
}

std::string selfTest(uint32_t* next, uint32_t chunk) {
    if (*next == 0) {
        if (hsvToRgb({0.0, 1.0, 1.0}) != Rgb8{255, 0, 0} ||
            hsvToRgb({120.0, 0.5, 0.8}) != Rgb8{0x66, 0xCC, 0x66} ||
            hsvToRgb({240.0, 1.0, 1.0}) != Rgb8{0, 0, 255})
        {
            return "hsvToRgb primaries";
        }
        if (parseHex(" #3fa34D ") != Rgb8{0x3F, 0xA3, 0x4D} || parseHex("abc") != Rgb8{0xAA, 0xBB, 0xCC} ||
            parseHex("#12345") || parseHex("zzzzzz"))
        {
            return "parseHex";
        }
        if (formatHex({0x3F, 0xA3, 0x4D}) != "#3FA34D" || formatConfig({0x3F, 0xA3, 0x4D}) != "3fa34d") {
            return "formatHex";
        }
        if (contrastOnTagFill({255, 255, 255}) < 15.0 ||
            std::abs(contrastOnTagFill({10, 12, 16}) - 1.0) > 1e-9)
        {
            return "contrastOnTagFill";
        }
        if (contrastOnTagFill(readableOnTagFill({0, 0, 0}, 3.0)) < 3.0) {
            return "readableOnTagFill";
        }
    }
    const uint32_t end = std::min<uint32_t>(*next + chunk, 1u << 24);
    for (uint32_t i = *next; i < end; ++i) {
        const Rgb8 c{static_cast<uint8_t>(i >> 16), static_cast<uint8_t>(i >> 8),
            static_cast<uint8_t>(i)};
        if (hsvToRgb(rgbToHsv(c, {0.0, 0.0, 1.0})) != c) {
            return fmt::format("{} does not survive rgb -> hsv -> rgb", formatHex(c));
        }
        if (parseHex(formatConfig(c)) != c) {
            return fmt::format("{} does not survive the config format", formatHex(c));
        }
    }
    *next = end;
    return {};
}

}  // namespace twili::ui::color
