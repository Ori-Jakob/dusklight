#include "fx/PlayerRecolor.hpp"

#include "core/Log.hpp"
#include "core/PrivateAccess.hpp"

#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JUtility/JUTNameTab.h"
#include "d/d_kankyo_tev_str.h"

#include <fmt/format.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace twili {
namespace {

// Tunic hue bands measured on the disc's textures; the per-set whitelist keeps pants out.
constexpr HueBand kGreen{100, 45, 15, 0.10f, 0.04f};  // Kokiri tunic and hat
constexpr HueBand kBlue{205, 45, 15, 0.10f, 0.04f};   // Zora mask
constexpr HueBand kRed{350, 25, 10, 0.22f, 0.06f};    // Magic Armor cloth
constexpr HueBand kOlive{85, 25, 10, 0.18f, 0.05f};   // Ordon tunic

constexpr RecolorTexSpec kKokiriBody[] = {
    {"al_upbody_m", 256, 128, RecolorMode::Band, kGreen, 1.0f},
    {"al_skirt_m", 64, 32, RecolorMode::Band, kGreen, 1.0f},
    {"al_lowbody_m", 128, 128, RecolorMode::Band, kGreen, 1.0f},
};
constexpr RecolorTexSpec kKokiriHead[] = {
    {"al_cap_m", 128, 128, RecolorMode::Band, kGreen, 1.0f},
};
// NotWarm leaves zl_body's brown plates, tan wraps and gold alone.
constexpr RecolorTexSpec kZoraBody[] = {
    {"zl_uroko_m", 256, 128, RecolorMode::NotWarm, {}, 1.0f},
    {"zl_up_armor_m", 128, 128, RecolorMode::NotWarm, {}, 1.0f},
    {"zl_mask_m", 32, 32, RecolorMode::Band, kBlue, 1.0f},
    {"zl_bootsA_m", 64, 64, RecolorMode::NotWarm, {}, 1.0f},
};
constexpr RecolorTexSpec kZoraHead[] = {
    {"zl_cap_m", 64, 128, RecolorMode::NotWarm, {}, 1.0f},
};
constexpr RecolorTexSpec kMagicBody[] = {
    {"ml_body_m", 128, 128, RecolorMode::Band, kRed, 1.0f},
    {"al_bag_m", 128, 128, RecolorMode::Band, kRed, 1.0f},
};
constexpr RecolorTexSpec kMagicHead[] = {
    {"ml_cap_m", 128, 128, RecolorMode::Band, kRed, 1.0f},
};
// The olive tunic over the cream shirt; the band alone speckles the shirt's embroidery.
constexpr BlockRect kOrdonTunic{0, 8, 39, 31};
constexpr RecolorTexSpec kOrdonBody[] = {
    {"bl_upbody_m", 256, 128, RecolorMode::Band, kOlive, 1.0f, nullptr, 0, &kOrdonTunic},
};
// wl_body block rows of the tail's UV island; column 60 also holds texels of the body's.
constexpr BlockSpan kWolfTail[] = {
    {1, 65, 93},  {2, 61, 96},  {3, 61, 98},  {4, 61, 100}, {5, 61, 103}, {6, 61, 105},
    {7, 61, 105}, {8, 61, 103}, {9, 61, 101}, {10, 61, 99}, {11, 61, 97}, {12, 61, 93},
    {13, 62, 89}, {14, 66, 85}, {15, 70, 79}, {16, 73, 77},
};
constexpr float kWolfStrength = 0.9f;
constexpr RecolorTexSpec kWolfBody[] = {
    {"wl_body_m", 512, 512, RecolorMode::Colorize, {}, kWolfStrength, kWolfTail,
     static_cast<uint8_t>(std::size(kWolfTail))},
    {"wl_legbond_m", 32, 32, RecolorMode::Colorize, {}, kWolfStrength},  // the ankle shackle
};
constexpr RecolorTexSpec kWolfChain[] = {
    {"wl_kusari_m", 32, 32, RecolorMode::Colorize, {}, kWolfStrength},
};

constexpr RecolorSetSpec kSets[static_cast<size_t>(RecolorSetId::Count)] = {
    {"kokiri", {{kKokiriBody, 3}, {kKokiriHead, 1}}},
    {"zora", {{kZoraBody, 4}, {kZoraHead, 1}}},
    {"magic", {{kMagicBody, 2}, {kMagicHead, 1}}},
    {"ordon", {{kOrdonBody, 1}, {nullptr, 0}}},
    {"wolf", {{kWolfBody, 2}, {kWolfChain, 1}}},
};

constexpr uint32_t kBlockBytes = 8;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

uint32_t fnv1a(uint32_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

constexpr uint32_t kFnvBasis = 2166136261u;

// Block `b` of a CMPR image in 4x4-block units: 8x8 tiles in row-major order, four blocks each.
void blockPos(uint32_t b, uint32_t width, uint32_t& col, uint32_t& row) {
    const uint32_t tile = b >> 2, sub = b & 3, tilesX = width >> 3;
    col = (tile % tilesX) * 2 + (sub & 1);
    row = (tile / tilesX) * 2 + (sub >> 1);
}

bool inRegion(const RecolorTexSpec& spec, uint32_t col, uint32_t row) {
    if (spec.spans != nullptr) {
        for (uint8_t i = 0; i < spec.spanCount; i++) {
            const BlockSpan& s = spec.spans[i];
            if (s.row == row) {
                return col >= s.col0 && col <= s.col1;
            }
        }
        return false;
    }
    if (spec.rect != nullptr) {
        const BlockRect& r = *spec.rect;
        return col >= r.col0 && col <= r.col1 && row >= r.row0 && row <= r.row1;
    }
    return true;
}

float endWeight(const RecolorTexSpec& spec, uint16_t v) {
    if (spec.mode == RecolorMode::Colorize) {
        return 1.0f;
    }
    return recolorWeight(spec.mode, spec.band, toHsv(from565(v)));
}

// Through the material that samples it: TEX1 has duplicate names, the materials use later ones.
bool findTexture(J3DModelData* data, const char* material, J3DTexture*& tex, uint16_t& texNo) {
    JUTNameTab* names = data != nullptr ? data->getMaterialName() : nullptr;
    const s32 mi = names != nullptr ? names->getIndex(material) : -1;
    if (mi < 0 || mi >= data->getMaterialNum()) {
        return false;
    }
    texNo = data->getMaterialNodePointer(static_cast<u16>(mi))->getTexNo(0);
    tex = data->getTexture();
    return tex != nullptr && texNo < tex->getNum();
}

Rgb keyRgb(uint32_t key) {
    return {static_cast<float>((key >> 16) & 0xFF) / 255.0f,
            static_cast<float>((key >> 8) & 0xFF) / 255.0f,
            static_cast<float>(key & 0xFF) / 255.0f};
}

}  // namespace

uint32_t recolorKey(uint8_t r, uint8_t g, uint8_t b) {
    // Exact white means the original colours (#F5F5F5 gives a white tunic).
    if (r == 255 && g == 255 && b == 255) {
        return RecolorSet::kPristine;
    }
    return static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
}

const char* recolorSetName(RecolorSetId id) {
    return id < RecolorSetId::Count ? kSets[static_cast<size_t>(id)].name : "?";
}

void RecolorSet::reset() {
    *this = RecolorSet{};
}

int RecolorSet::bind(RecolorSetId id, J3DModelData* const models[2], JKRHeap* heap,
                     uint32_t clientId) {
    reset();
    mId = id;
    mClientId = clientId;
    const RecolorSetSpec& set = kSets[static_cast<size_t>(id)];
    double acc[3] = {};
    double weightSum = 0.0;
    uint32_t blocksTotal = 0;
    for (int m = 0; m < 2; m++) {
        const RecolorModelSpec& modelSpec = set.models[m];
        for (uint8_t i = 0; models[m] != nullptr && i < modelSpec.count; i++) {
            const RecolorTexSpec& spec = modelSpec.tex[i];
            J3DTexture* tex = nullptr;
            uint16_t texNo = 0;
            if (!findTexture(models[m], spec.material, tex, texNo)) {
                TwiliLog.warn("[recolor {}] {}/{} does not match (no such material), left vanilla",
                              clientId, set.name, spec.material);
                continue;
            }
            const ResTIMG* timg = tex->getResTIMG(texNo);
            const u16 width = timg->width, height = timg->height;
            const char* why = nullptr;
            if (timg->format != GX_TF_CMPR || timg->indexTexture) {
                why = "format";
            } else if (width != spec.width || height != spec.height || width % 8 || height % 8) {
                why = "size";
            } else if (timg->mipmapCount > 1) {
                why = "mipmaps";
            }
            uint8_t* image = tex->getImgDataPtr(texNo);
            for (uint8_t k = 0; why == nullptr && k < mCount; k++) {
                if (mTex[k].image == image) {
                    why = "image already bound";
                }
            }
            if (why == nullptr && mCount >= std::size(mTex)) {
                why = "too many textures";
            }
            if (why != nullptr) {
                TwiliLog.warn("[recolor {}] {}/{} does not match ({}), left vanilla", clientId,
                              set.name, spec.material, why);
                continue;
            }

            const uint32_t blockCount = (width / 4u) * (height / 4u);
            auto isCandidate = [&](uint32_t b) {
                uint32_t col, row;
                blockPos(b, width, col, row);
                if (!inRegion(spec, col, row)) {
                    return false;
                }
                if (spec.mode == RecolorMode::Colorize) {
                    return true;
                }
                const uint8_t* blk = image + b * kBlockBytes;
                return endWeight(spec, be16(blk)) > 0.0f || endWeight(spec, be16(blk + 2)) > 0.0f;
            };
            uint32_t count = 0;
            for (uint32_t b = 0; b < blockCount; b++) {
                if (!isCandidate(b)) {
                    continue;
                }
                count++;
                if (spec.mode == RecolorMode::Colorize) {
                    continue;
                }
                // The set's reference: the weighted mean of its tunic-coloured end colours.
                for (int e = 0; e < 2; e++) {
                    const uint16_t v = be16(image + b * kBlockBytes + e * 2);
                    const float w = endWeight(spec, v);
                    const Rgb c = from565(v);
                    acc[0] += c.r * w;
                    acc[1] += c.g * w;
                    acc[2] += c.b * w;
                    weightSum += w;
                }
            }
            if (count == 0) {
                TwiliLog.warn("[recolor {}] {}/{} has nothing to recolour, left vanilla", clientId,
                              set.name, spec.material);
                continue;
            }
            const uint32_t storeBytes = count * (kBlockBytes + sizeof(uint16_t));
            auto* store = static_cast<uint8_t*>(JKRHeap::alloc(storeBytes, 4, heap));
            if (store == nullptr) {
                TwiliLog.warn("[recolor {}] {}/{}: no heap for 0x{:X} bytes, left vanilla",
                              clientId, set.name, spec.material, storeBytes);
                continue;
            }
            Tex& t = mTex[mCount++];
            t = {};
            t.spec = &spec;
            t.tex = tex;
            t.texNo = texNo;
            t.blockCount = static_cast<uint16_t>(blockCount);
            t.image = image;
            t.pristine = store;
            t.blocks = reinterpret_cast<uint16_t*>(store + count * kBlockBytes);
            t.count = static_cast<uint16_t>(count);
            uint32_t k = 0;
            for (uint32_t b = 0; b < blockCount; b++) {
                if (isCandidate(b)) {
                    t.blocks[k] = static_cast<uint16_t>(b);
                    std::memcpy(t.pristine + k * kBlockBytes, image + b * kBlockBytes, kBlockBytes);
                    k++;
                }
            }
            t.untouchedHash = untouchedHash(t);
            mBytes += storeBytes;
            blocksTotal += count;
        }
    }
    if (weightSum > 0.0) {
        mRef = toHsv({static_cast<float>(acc[0] / weightSum),
                      static_cast<float>(acc[1] / weightSum),
                      static_cast<float>(acc[2] / weightSum)});
    }
    if (mCount != 0) {
        TwiliLog.info("[recolor {}] {} bound {} textures, {} blocks, 0x{:X} bytes, ref h{:.1f} "
                      "s{:.3f} v{:.3f}",
                      clientId, set.name, mCount, blocksTotal, mBytes, mRef.h, mRef.s, mRef.v);
    }
    return mCount;
}

// FNV-1a of the blocks no recolour ever writes, in image order.
uint32_t RecolorSet::untouchedHash(const Tex& t) const {
    uint32_t h = kFnvBasis;
    uint32_t k = 0;
    for (uint32_t b = 0; b < t.blockCount; b++) {
        if (k < t.count && t.blocks[k] == b) {
            k++;
            continue;
        }
        h = fnv1a(h, t.image + b * kBlockBytes, kBlockBytes);
    }
    return h;
}

// The textures are the dummy's private copies: rewritten in place, then decoded again.
void RecolorSet::apply(uint32_t key, int32_t tick) {
    const auto start = std::chrono::steady_clock::now();
    RecolorParams p{};
    p.ref = mRef;
    p.target = toHsv(keyRgb(key));
    uint32_t blocks = 0;
    for (uint8_t i = 0; i < mCount; i++) {
        Tex& t = mTex[i];
        p.mode = t.spec->mode;
        p.band = t.spec->band;
        p.strength = t.spec->strength;
        for (uint16_t k = 0; k < t.count; k++) {
            const uint8_t* src = t.pristine + k * kBlockBytes;
            uint8_t* dst = t.image + t.blocks[k] * kBlockBytes;
            if (key == kPristine) {
                std::memcpy(dst, src, kBlockBytes);
                continue;
            }
            const uint16_t c0 = be16(src), c1 = be16(src + 2);
            storeCmprBlock(dst, recolor565(c0, p), recolor565(c1, p), src + 4, c0 > c1);
        }
        blocks += t.count;
        // Every TEX1 entry that shows this image (wl_body has two).
        for (u16 n = 0; n < t.tex->getNum(); n++) {
            if (t.tex->getImgDataPtr(n) == t.image) {
                notifyTextureDataChanged(*t.tex, n);
                mRefreshCount++;
            }
        }
    }
    mAppliedKey = key;
    mLastApplyTick = tick;
    mApplyCount++;
    mLastApplyMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                            start)
                       .count();
    TwiliLog.debug("[recolor {}] {} -> {}, {} blocks, {:.2f} ms", mClientId,
                   kSets[static_cast<size_t>(mId)].name,
                   key == kPristine ? std::string("pristine") : fmt::format("#{:06X}", key),
                   blocks, mLastApplyMs);
}

void RecolorSet::probe(RecolorProbe& out) const {
    out.bound = bound();
    out.appliedKey = mAppliedKey;
    out.applyCount = mApplyCount;
    out.lastApplyMs = mLastApplyMs;
    out.texCount = mCount;
    for (uint8_t i = 0; i < mCount && i < std::size(out.tex); i++) {
        const Tex& t = mTex[i];
        RecolorTexProbe& tp = out.tex[i];
        tp = {};
        tp.material = t.spec->material;
        tp.candidates = t.count;
        tp.untouchedIntact = untouchedHash(t) == t.untouchedHash;
        tp.pristine = true;
        tp.refreshes = mRefreshCount;
        double acc[3] = {};
        uint32_t n = 0;
        for (uint16_t k = 0; k < t.count; k++) {
            const uint8_t* src = t.pristine + k * kBlockBytes;
            const uint8_t* live = t.image + t.blocks[k] * kBlockBytes;
            if (std::memcmp(src, live, kBlockBytes) != 0) {
                tp.pristine = false;
            }
            // Both ends fully tunic-coloured, so a reordered block still reads right.
            if (endWeight(*t.spec, be16(src)) < 0.99f ||
                endWeight(*t.spec, be16(src + 2)) < 0.99f)
            {
                continue;
            }
            uint16_t ends[2] = {be16(live), be16(live + 2)};
            if (ends[1] == 0 && ends[0] != 0 && std::memcmp(live + 4, "\0\0\0\0", 4) == 0) {
                ends[1] = ends[0];  // a flat block parks c1 at black, unused
            }
            for (uint16_t e : ends) {
                const Rgb c = from565(e);
                acc[0] += c.r;
                acc[1] += c.g;
                acc[2] += c.b;
                n++;
            }
        }
        if (n != 0) {
            const Hsv mean = toHsv({static_cast<float>(acc[0] / n), static_cast<float>(acc[1] / n),
                                    static_cast<float>(acc[2] / n)});
            tp.meanH = mean.h;
            tp.meanS = mean.s;
            tp.meanV = mean.v;
        }
    }
}

bool RecolorPalette::bind(J3DModelData* data, const char* material, uint16_t numColors,
                          JKRHeap* heap, uint32_t clientId) {
    *this = RecolorPalette{};
    J3DTexture* tex = nullptr;
    uint16_t texNo = 0;
    if (!findTexture(data, material, tex, texNo)) {
        TwiliLog.warn("[recolor {}] horse/{} does not match (no such material), left vanilla",
                      clientId, material);
        return false;
    }
    const ResTIMG* timg = tex->getResTIMG(texNo);
    if (timg->format != GX_TF_C8 || !timg->indexTexture || timg->colorFormat != GX_TL_RGB5A3 ||
        timg->numColors != numColors)
    {
        TwiliLog.warn("[recolor {}] horse/{} does not match (palette), left vanilla", clientId,
                      material);
        return false;
    }
    auto* pristine = static_cast<uint8_t*>(JKRHeap::alloc(numColors * 2u, 4, heap));
    if (pristine == nullptr) {
        TwiliLog.warn("[recolor {}] horse/{}: no heap, left vanilla", clientId, material);
        return false;
    }
    mTex = tex;
    mTexNo = texNo;
    mLive = textureTlutData(*tex, texNo);
    mPristine = pristine;
    mNum = numColors;
    std::memcpy(mPristine, mLive, numColors * 2u);
    return true;
}

void RecolorPalette::apply(uint32_t key, float strength) {
    if (!bound() || key == mAppliedKey) {
        return;
    }
    if (key == RecolorSet::kPristine) {
        std::memcpy(mLive, mPristine, mNum * 2u);
    } else {
        const RecolorParams p{RecolorMode::Colorize, {}, strength, {}, toHsv(keyRgb(key))};
        for (uint16_t i = 0; i < mNum; i++) {
            putBe16(mLive + i * 2, recolor5A3(be16(mPristine + i * 2), p));
        }
    }
    notifyTextureDataChanged(*mTex, mTexNo);
    mAppliedKey = key;
}

bool RecolorPalette::probe(Hsv& live, Hsv& pristine) const {
    if (!bound()) {
        return false;
    }
    auto mean = [this](const uint8_t* pal) {
        double acc[3] = {};
        int n = 0;
        for (uint16_t i = 0; i < mNum; i++) {
            const uint16_t v = be16(pal + i * 2);
            if (!(v & 0x8000)) {
                continue;  // RGB4A3: the translucent fringe
            }
            acc[0] += ((v >> 10) & 31) / 31.0;
            acc[1] += ((v >> 5) & 31) / 31.0;
            acc[2] += (v & 31) / 31.0;
            n++;
        }
        return n == 0 ? Hsv{} : toHsv({static_cast<float>(acc[0] / n),
                                       static_cast<float>(acc[1] / n),
                                       static_cast<float>(acc[2] / n)});
    };
    live = mean(mLive);
    pristine = mean(mPristine);
    return true;
}

bool bindHorseRecolor(HorseRecolor& st, J3DModelData* hs, JKRHeap* heap, uint32_t clientId) {
    const bool mane = st.mane.bind(hs, "hair_m", 160, heap, clientId);
    if (kTintHorseTail) {
        st.tail.bind(hs, "nuki_m", 208, heap, clientId);
    }
    return mane;
}

void updateHorseRecolor(HorseRecolor& st, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t key = recolorKey(r, g, b);
    // A grey has no hue to tint with: the mane stays as it is.
    if (key != RecolorSet::kPristine && toHsv(keyRgb(key)).s < 0.10f) {
        key = RecolorSet::kPristine;
    }
    st.mane.apply(key, kHorseManeStrength);
    if (kTintHorseTail) {
        st.tail.apply(key, kHorseManeStrength);
    }
}

void applyHorseColor(const HorseRecolor& st, J3DModelData* data, const dKy_tevstr_c& tevStr,
                     uint8_t r, uint8_t g, uint8_t b) {
    (void)st;
    // head_m is the coat: a chroma-only offset on top of what MAJI wrote to C0; greys leave it.
    constexpr u16 kHeadMaterial = 3;
    constexpr int kDivisor = kHorseBodyTintDivisor > 0 ? kHorseBodyTintDivisor : 1;
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (kHorseBodyTintDivisor <= 0 || data == nullptr || data->getMaterialNum() <= kHeadMaterial ||
        mx - mn < 12)
    {
        return;
    }
    const int avg = (r + g + b) / 3;
    auto clampS10 = [](int v) { return static_cast<s16>(std::clamp(v, -1024, 1023)); };
    J3DGXColorS10 c(tevStr.TevColor);
    c.r = clampS10(c.r + (r - avg) / kDivisor);
    c.g = clampS10(c.g + (g - avg) / kDivisor);
    c.b = clampS10(c.b + (b - avg) / kDivisor);
    data->getMaterialNodePointer(kHeadMaterial)->setTevColor(0, &c);
}

std::string recolorSelfTest() {
    // storeCmprBlock keeps each block's mode.
    struct ModeCase {
        uint16_t n0, n1;
        uint8_t idx;
        bool four;
        uint16_t w0, w1;
        uint8_t wIdx;
    };
    // idx 0x1B = texels 0,1,2,3 in a row.
    static constexpr ModeCase kModes[] = {
        {0x1000, 0x2000, 0x1B, true, 0x2000, 0x1000, 0x4E},  // 4-colour, swapped: 1,0,3,2
        {0x3000, 0x3000, 0x1B, true, 0x3000, 0x0000, 0x00},  // 4-colour, equal ends: flat
        {0x0000, 0x0000, 0x1B, true, 0x0001, 0x0000, 0x00},  // 4-colour, black: (1, 0)
        {0x2000, 0x1000, 0x1B, false, 0x1000, 0x2000, 0x4B},  // 3-colour, swapped: 1,0,2,3
        {0x1000, 0x2000, 0x1B, false, 0x1000, 0x2000, 0x1B},  // 3-colour, kept
        {0x2000, 0x1000, 0x1B, true, 0x2000, 0x1000, 0x1B},   // 4-colour, kept
    };
    for (const ModeCase& c : kModes) {
        uint8_t blk[8];
        const uint8_t idx[4] = {c.idx, c.idx, c.idx, c.idx};
        storeCmprBlock(blk, c.n0, c.n1, idx, c.four);
        if (be16(blk) != c.w0 || be16(blk + 2) != c.w1 || blk[4] != c.wIdx || blk[7] != c.wIdx) {
            return fmt::format("storeCmprBlock({:04X}, {:04X}, {}) gave {:04X} {:04X} {:02X}", c.n0,
                               c.n1, c.four ? "four" : "three", be16(blk), be16(blk + 2), blk[4]);
        }
        const bool four = be16(blk) > be16(blk + 2);
        if (four != c.four) {
            return fmt::format("storeCmprBlock({:04X}, {:04X}) changed the block's mode", c.n0,
                               c.n1);
        }
    }

    // Weight 0 is the identity, bit for bit: a brown lace in a Kokiri texture.
    const RecolorParams kokiriRed{RecolorMode::Band, kGreen, 1.0f, {98.8f, 0.308f, 0.211f},
                                  toHsv({229 / 255.0f, 57 / 255.0f, 53 / 255.0f})};
    for (uint16_t v : {uint16_t(0x6A45), uint16_t(0x8B0A), uint16_t(0xFFFF), uint16_t(0x0000)}) {
        if (recolor565(v, kokiriRed) != v) {
            return fmt::format("recolor565({:04X}) moved an end colour outside the band", v);
        }
    }

    // Golden vectors from the offline prototype; C++ and Python agree within 1 LSB per channel.
    struct Golden {
        RecolorMode mode;
        const HueBand* band;
        float strength;
        Hsv ref;
        uint8_t r, g, b;
        uint16_t in, out;
    };
    constexpr Hsv kKokiriRef{98.8f, 0.308f, 0.211f};
    constexpr Hsv kZoraRef{195.9f, 0.226f, 0.153f};
    static const Golden kGolden[] = {
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 229, 57, 53, 0x1923, 0x4882},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 229, 57, 53, 0x3A05, 0x88E3},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 229, 57, 53, 0x4A87, 0xA965},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 30, 136, 229, 0x2164, 0x11CB},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 30, 136, 229, 0x29C5, 0x124E},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 0, 0, 0, 0x3A26, 0x18C3},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 245, 245, 245, 0x3A05, 0x94B2},
        {RecolorMode::Band, &kGreen, 1.0f, kKokiriRef, 245, 245, 245, 0x4A87, 0xB5D6},
        {RecolorMode::Colorize, nullptr, 0.9f, {}, 229, 57, 53, 0x4208, 0x40C3},
        {RecolorMode::Colorize, nullptr, 0.9f, {}, 229, 57, 53, 0x8410, 0x7986},
        {RecolorMode::Colorize, nullptr, 0.9f, {}, 229, 57, 53, 0x2945, 0x2882},
        {RecolorMode::NotWarm, nullptr, 1.0f, kZoraRef, 229, 57, 53, 0x2186, 0x88C3},
        {RecolorMode::NotWarm, nullptr, 1.0f, kZoraRef, 229, 57, 53, 0x4A69, 0xDAEB},
        {RecolorMode::NotWarm, nullptr, 1.0f, kZoraRef, 229, 57, 53, 0x8B0A, 0x8B0A},
    };
    for (const Golden& g : kGolden) {
        const RecolorParams p{g.mode, g.band != nullptr ? *g.band : HueBand{}, g.strength, g.ref,
                              toHsv({g.r / 255.0f, g.g / 255.0f, g.b / 255.0f})};
        const uint16_t have = recolor565(g.in, p);
        const int dr = std::abs(((have >> 11) & 31) - ((g.out >> 11) & 31));
        const int dg = std::abs(((have >> 5) & 63) - ((g.out >> 5) & 63));
        const int db = std::abs((have & 31) - (g.out & 31));
        if (dr > 1 || dg > 1 || db > 1) {
            return fmt::format("recolor565({:04X}) to #{:02X}{:02X}{:02X} gave {:04X}, want {:04X}",
                               g.in, g.r, g.g, g.b, have, g.out);
        }
    }

    // HSV round trip on the primaries and a grey.
    for (const Rgb& c : {Rgb{1, 0, 0}, Rgb{0, 1, 0}, Rgb{0, 0, 1}, Rgb{0.5f, 0.5f, 0.5f},
                         Rgb{0.9f, 0.2f, 0.6f}})
    {
        const Rgb back = fromHsv(toHsv(c));
        if (std::fabs(back.r - c.r) > 1e-5f || std::fabs(back.g - c.g) > 1e-5f ||
            std::fabs(back.b - c.b) > 1e-5f)
        {
            return fmt::format("HSV round trip of ({}, {}, {}) failed", c.r, c.g, c.b);
        }
    }

    // The mane's palette: alpha bits kept, a colour moves it only slightly.
    const RecolorParams mane{RecolorMode::Colorize, {}, kHorseManeStrength, {},
                             toHsv({229 / 255.0f, 57 / 255.0f, 53 / 255.0f})};
    const uint16_t translucent = 0x3A85;  // alpha 3, RGB444 (A, 8, 5)
    if ((recolor5A3(translucent, mane) & 0xF000) != (translucent & 0xF000)) {
        return "recolor5A3 changed a palette entry's alpha";
    }
    const uint16_t opaque = 0x8000 | 18 << 10 | 14 << 5 | 11;  // a mane brown
    const uint16_t tinted = recolor5A3(opaque, mane);
    if (!(tinted & 0x8000) || tinted == opaque ||
        std::abs(((tinted >> 10) & 31) - 18) > 4 || std::abs(((tinted >> 5) & 31) - 14) > 4)
    {
        return fmt::format("recolor5A3({:04X}) gave {:04X}: not a slight tint", opaque, tinted);
    }
    if (recolorKey(255, 255, 255) != RecolorSet::kPristine ||
        recolorKey(245, 245, 245) == RecolorSet::kPristine)
    {
        return "recolorKey: only exact white means the original colours";
    }
    return {};
}

}  // namespace twili
