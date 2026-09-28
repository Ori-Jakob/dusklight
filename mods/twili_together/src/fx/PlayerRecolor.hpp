#pragma once

// Recolours a remote player's clothes in the player's colour (PlayerRecolor.cpp).

#include "fx/PlayerRecolorMath.hpp"

#include <cstdint>
#include <string>

class J3DModelData;
class J3DTexture;
class JKRHeap;
class dKy_tevstr_c;

namespace twili {

enum class RecolorSetId : uint8_t { Kokiri, Zora, Magic, Ordon, Wolf, Count };

struct BlockSpan {
    uint8_t row, col0, col1;  // 4x4-block units, inclusive
};
struct BlockRect {
    uint16_t col0, row0, col1, row1;  // 4x4-block units, inclusive
};

struct RecolorTexSpec {
    const char* material;    // its TEXMAP0 is the texture (TEX1 has duplicate names)
    uint16_t width, height;  // expected, checked at bind
    RecolorMode mode;
    HueBand band;    // Band only
    float strength;  // Colorize; 1 otherwise
    const BlockSpan* spans = nullptr;  // optional region
    uint8_t spanCount = 0;
    const BlockRect* rect = nullptr;   // optional region
};
struct RecolorModelSpec {
    const RecolorTexSpec* tex;
    uint8_t count;
};
struct RecolorSetSpec {
    const char* name;
    RecolorModelSpec models[2];  // body; hat or chain
};

// What the autotest reads (GetDummyPlayerRecolorProbe).
struct RecolorTexProbe {
    const char* material = nullptr;
    uint16_t candidates = 0;
    bool untouchedIntact = false;  // the blocks that never change still hash as at bind
    bool pristine = false;         // every candidate block equals its pristine copy
    float meanH = 0.0f, meanS = 0.0f, meanV = 0.0f;
    uint32_t refreshes = 0;  // notifyDataChanged calls
};
struct RecolorProbe {
    int8_t set = -1;  // RecolorSetId worn
    bool bound = false;
    uint32_t appliedKey = 0, applyCount = 0;
    float lastApplyMs = 0.0f;
    uint32_t bytes = 0;      // pristine stores of every set of this dummy
    uint32_t draws = 0;
    int16_t c1MaxAbs = 0;    // max |C1.rgb| over the worn set's wet-cloth materials
    uint8_t texCount = 0;
    RecolorTexProbe tex[6];
};

class RecolorSet {
public:
    static constexpr uint32_t kPristine = 0xFF000000u;  // the bytes as on disc

    // createHeap only
    int bind(RecolorSetId id, J3DModelData* const models[2], JKRHeap* heap, uint32_t clientId);
    void reset();  // the heap the store lived in is gone
    bool bound() const { return mCount != 0; }
    uint32_t appliedKey() const { return mAppliedKey; }
    int32_t lastApplyTick() const { return mLastApplyTick; }
    // Rewrites the candidate blocks for `key` (0x00RRGGBB or kPristine) and notifies J3D.
    void apply(uint32_t key, int32_t tick);
    uint32_t bytes() const { return mBytes; }
    void probe(RecolorProbe& out) const;

private:
    struct Tex {
        const RecolorTexSpec* spec;
        J3DTexture* tex;
        uint16_t texNo;
        uint16_t blockCount;  // of the whole image
        uint8_t* image;
        uint16_t* blocks;     // candidate block numbers, ascending
        uint8_t* pristine;    // 8 bytes per candidate
        uint16_t count;
        uint32_t untouchedHash;
    };
    uint32_t untouchedHash(const Tex& t) const;

    Tex mTex[6];
    uint8_t mCount = 0;
    RecolorSetId mId{};
    uint32_t mClientId = 0;
    Hsv mRef{};
    uint32_t mAppliedKey = kPristine;
    uint32_t mApplyCount = 0;
    uint32_t mRefreshCount = 0;
    uint32_t mBytes = 0;
    int32_t mLastApplyTick = -1000;
    float mLastApplyMs = 0.0f;
};

// kPristine for the default white (the original colours)
uint32_t recolorKey(uint8_t r, uint8_t g, uint8_t b);
const char* recolorSetName(RecolorSetId id);

// One C8 texture's palette (Epona's mane), tinted in place.
class RecolorPalette {
public:
    bool bind(J3DModelData* data, const char* material, uint16_t numColors, JKRHeap* heap,
              uint32_t clientId);
    void apply(uint32_t key, float strength);  // kPristine restores
    bool bound() const { return mLive != nullptr; }
    uint32_t appliedKey() const { return mAppliedKey; }
    bool probe(Hsv& live, Hsv& pristine) const;

private:
    J3DTexture* mTex = nullptr;
    uint16_t mTexNo = 0;
    uint8_t* mLive = nullptr;
    uint8_t* mPristine = nullptr;
    uint16_t mNum = 0;
    uint32_t mAppliedKey = RecolorSet::kPristine;
};

// The remote Epona's colour (for its puppet, a separate feature)
struct HorseRecolor {
    RecolorPalette mane;  // hair_m -> hs_hair (C8, 160 x RGB5A3)
    RecolorPalette tail;
};
inline constexpr float kHorseManeStrength = 0.30f;  // "a slight tint"
inline constexpr bool kTintHorseTail = false;
inline constexpr int kHorseBodyTintDivisor = 0;  // 0: no C0 offset on the coat (head_m)

bool bindHorseRecolor(HorseRecolor& st, J3DModelData* hs, JKRHeap* heap, uint32_t clientId);
// Once per tick with the owner's colour (Client::colorR/G/B).
void updateHorseRecolor(HorseRecolor& st, uint8_t r, uint8_t g, uint8_t b);
// Every draw, right after setLightTevColorType_MAJI.
void applyHorseColor(const HorseRecolor& st, J3DModelData* data, const dKy_tevstr_c& tevStr,
                     uint8_t r, uint8_t g, uint8_t b);

// The maths and block-mode vectors (autotest recolorSelfTest).
std::string recolorSelfTest();

}  // namespace twili

