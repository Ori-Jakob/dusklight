#pragma once

// Colour maths of the remote player recolour (PlayerRecolor.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace twili {

struct Rgb {
    float r, g, b;
};
struct Hsv {
    float h, s, v;  // h in degrees [0, 360), s and v 0..1
};

// Which end colours a texture's recolour takes
struct HueBand {
    float center, halfWidth, feather, satMin, satFeather;
};

enum class RecolorMode : uint8_t {
    Band,      // end colours inside the texture's hue band
    NotWarm,   // everything but brown, tan and gold (kWarm)
    Colorize,
};

inline constexpr HueBand kWarm{35, 30, 10, 0.15f, 0.05f};

inline float smooth01(float e0, float e1, float x) {
    float t = e1 != e0 ? (x - e0) / (e1 - e0) : (x >= e0 ? 1.0f : 0.0f);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float hueDist(float a, float b) {
    const float d = std::fmod(std::fabs(a - b), 360.0f);
    return std::min(d, 360.0f - d);
}

// Expanded exactly like aurora's CMPR decoder (ExpandTo8).
inline Rgb from565(uint16_t v) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {static_cast<float>(r << 3 | r >> 2) / 255.0f,
            static_cast<float>(g << 2 | g >> 4) / 255.0f,
            static_cast<float>(b << 3 | b >> 2) / 255.0f};
}

inline int quantize(float c, int max) {
    return static_cast<int>(std::lround(std::clamp(c, 0.0f, 1.0f) * static_cast<float>(max)));
}

inline uint16_t to565(const Rgb& c) {
    return static_cast<uint16_t>(quantize(c.r, 31) << 11 | quantize(c.g, 63) << 5 |
                                 quantize(c.b, 31));
}

// The hexcone model, as Python's colorsys (the offline prototype the vectors come from).
inline Hsv toHsv(const Rgb& c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    if (mx == mn) {
        return {0.0f, 0.0f, mx};
    }
    const float range = mx - mn;
    const float rc = (mx - c.r) / range, gc = (mx - c.g) / range, bc = (mx - c.b) / range;
    float h = c.r == mx ? bc - gc : c.g == mx ? 2.0f + rc - bc : 4.0f + gc - rc;
    h = std::fmod(h * 60.0f, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    return {h, range / mx, mx};
}

inline Rgb fromHsv(const Hsv& c) {
    if (c.s == 0.0f) {
        return {c.v, c.v, c.v};
    }
    const float h6 = c.h / 60.0f;
    const int i = static_cast<int>(h6);
    const float f = h6 - static_cast<float>(i);
    const float p = c.v * (1.0f - c.s), q = c.v * (1.0f - c.s * f),
                t = c.v * (1.0f - c.s * (1.0f - f));
    switch (i % 6) {
    case 0: return {c.v, t, p};
    case 1: return {q, c.v, p};
    case 2: return {p, c.v, t};
    case 3: return {p, q, c.v};
    case 4: return {t, p, c.v};
    default: return {c.v, p, q};
    }
}

inline Rgb lerp(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

inline float bandWeight(const Hsv& c, const HueBand& b) {
    return (1.0f - smooth01(b.halfWidth, b.halfWidth + b.feather, hueDist(c.h, b.center))) *
           smooth01(b.satMin - b.satFeather, b.satMin + b.satFeather, c.s);
}

// How much of the recolour an end colour takes.
inline float recolorWeight(RecolorMode mode, const HueBand& band, const Hsv& c) {
    switch (mode) {
    case RecolorMode::Band: return bandWeight(c, band);
    case RecolorMode::NotWarm: return 1.0f - bandWeight(c, kWarm);
    default: return 1.0f;  // Colorize: the region decides
    }
}

struct RecolorParams {
    RecolorMode mode;
    HueBand band;
    float strength;  // Colorize; 1 otherwise
    Hsv ref;         // the set's reference colour: its average cloth colour
    Hsv target;      // the player's colour
};

// `c` moved towards the target, or `c` itself when its weight is 0.
inline Rgb recolorRgb(const Rgb& c, const RecolorParams& p, bool* changed = nullptr) {
    const Hsv h = toHsv(c);
    float w = recolorWeight(p.mode, p.band, h);
    if (w <= 0.0f) {
        if (changed) *changed = false;
        return c;
    }
    Hsv n{p.target.h, 0.0f, 0.0f};
    if (p.mode == RecolorMode::Colorize) {
        n.s = std::min(1.0f, p.target.s * p.strength);
        n.v = std::min(1.0f, h.v * (0.55f + 0.45f * p.target.v));
        w *= p.strength;
    } else {
        // Value is scaled, not replaced, so folds and seams keep their ratios
        const float satRatio = std::min(1.5f, h.s / std::max(p.ref.s, 1e-3f));
        n.s = std::min(1.0f, p.target.s * (0.75f + 0.25f * satRatio));
        n.v = std::min(1.0f, h.v * std::max(p.target.v, 0.15f) * 0.5f / std::max(p.ref.v, 1e-3f));
    }
    if (changed) *changed = true;
    return lerp(c, fromHsv(n), w);
}

// The new end colour, or `v` itself, bit for bit, when its weight is 0.
inline uint16_t recolor565(uint16_t v, const RecolorParams& p) {
    bool changed = false;
    const Rgb out = recolorRgb(from565(v), p, &changed);
    return changed ? to565(out) : v;
}

// An RGB5A3 palette entry: RGB555 with bit 15 set, else ARGB3444.
inline uint16_t recolor5A3(uint16_t v, const RecolorParams& p) {
    if (v & 0x8000) {
        const Rgb c{static_cast<float>((v >> 10) & 31) / 31.0f,
                    static_cast<float>((v >> 5) & 31) / 31.0f, static_cast<float>(v & 31) / 31.0f};
        bool changed = false;
        const Rgb n = recolorRgb(c, p, &changed);
        if (!changed) return v;
        return static_cast<uint16_t>(0x8000 | quantize(n.r, 31) << 10 | quantize(n.g, 31) << 5 |
                                     quantize(n.b, 31));
    }
    const Rgb c{static_cast<float>((v >> 8) & 15) / 15.0f,
                static_cast<float>((v >> 4) & 15) / 15.0f, static_cast<float>(v & 15) / 15.0f};
    bool changed = false;
    const Rgb n = recolorRgb(c, p, &changed);
    if (!changed) return v;
    return static_cast<uint16_t>((v & 0x7000) | quantize(n.r, 15) << 8 | quantize(n.g, 15) << 4 |
                                 quantize(n.b, 15));
}

// Writes a recoloured CMPR block.
inline void storeCmprBlock(uint8_t* blk, uint16_t n0, uint16_t n1, const uint8_t* idx, bool four) {
    uint8_t rows[4];
    std::memcpy(rows, idx, 4);
    if (four) {
        if (n0 == n1) {
            // Equal ends would switch to 3-colour mode.
            if (n0 == 0) n0 = 1;
            n1 = 0;
            std::memset(rows, 0, 4);  // flat block: every texel takes c0
        } else if (n0 < n1) {
            std::swap(n0, n1);
            for (uint8_t& r : rows) r ^= 0x55;  // index 0<->1, 2<->3
        }
    } else if (n0 > n1) {
        std::swap(n0, n1);
        for (uint8_t& r : rows) {
            r ^= (static_cast<uint8_t>(~r) >> 1) & 0x55;  // 0<->1; 2 (midpoint) and 3 (clear) stay
        }
    }
    blk[0] = static_cast<uint8_t>(n0 >> 8);
    blk[1] = static_cast<uint8_t>(n0 & 0xFF);
    blk[2] = static_cast<uint8_t>(n1 >> 8);
    blk[3] = static_cast<uint8_t>(n1 & 0xFF);
    std::memcpy(blk + 4, rows, 4);
}

}  // namespace twili
