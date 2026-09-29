#include "ui/NameTags.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "horse/HorsePuppet.hpp"
#include "horse/HorseSync.hpp"

#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JUtility/JUTResFont.h"
#include "JSystem/JUtility/TColor.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"

#include <mods/svc/gfx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace twili::ui::name_tags {
namespace {

struct TagLine {
    std::string text;
    f32 glyphW = 13.0f;
    f32 glyphH = 16.0f;
    f32 width = 0.0f;
};

struct TagDraw {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    f32 distanceSq = 0.0f;
    TagLine lines[2];
    int lineCount = 1;
    JUtility::TColor fill;
    JUtility::TColor border;
    JUtility::TColor text;
    JUtility::TColor text2;
};

constexpr f32 kGlyphWidth = 13.0f;
constexpr f32 kGlyphHeight = 16.0f;
// The horse's line and card: secondary to the player tags.
constexpr f32 kSmallGlyphWidth = 11.0f;
constexpr f32 kSmallGlyphHeight = 14.0f;
constexpr f32 kPadX = 8.0f;
constexpr f32 kPadY = 4.0f;
constexpr f32 kLineGap = 2.0f;
constexpr f32 kDotX = 7.0f;
constexpr f32 kDotSize = 4.0f;
// Left of the text: the dot plus a gap.
constexpr f32 kDotRoom = kDotX + kDotSize + 7.0f - kPadX;
constexpr size_t kMaxOwnerGlyphs = 24;
constexpr size_t kMaxHorseGlyphs = 16;
// A jump this far between two ticks is a teleport or respawn: no lerp across it.
constexpr f32 kSnapDistanceSq = 400.0f * 400.0f;

GfxStageHookHandle s_hook = 0;
std::vector<Probe> s_drawn;
uint32_t s_frames = 0;
const char* s_gate = "";

// The last two simulation positions of each tag's anchor, keyed by client (horses offset).
struct Track {
    uint64_t seq = 0;
    cXyz prev;
    cXyz cur;
};
std::unordered_map<uint64_t, Track> s_tracks;

cXyz presented(uint64_t key, const cXyz& now) {
    const uint64_t seq = interp::simTickSeq();
    auto [it, fresh] = s_tracks.try_emplace(key);
    Track& t = it->second;
    if (fresh) {
        t = {seq, now, now};
    } else if (seq != t.seq) {
        t.prev = t.cur;
        t.cur = now;
        t.seq = seq;
        if (t.prev.abs2(t.cur) > kSnapDistanceSq) {
            t.prev = t.cur;
        }
    } else {
        t.cur = now;
    }
    const f32 step = std::clamp(interp::presentationStep(), 0.0f, 1.0f);
    return t.prev + (t.cur - t.prev) * step;
}

// An unmapped code draws the font's default glyph.
bool fontHasGlyph(JUTFont* font, int code) {
    auto* res = static_cast<JUTResFont*>(font);
    return res->mInf1Ptr != nullptr &&
           res->getFontCode(code) != static_cast<int>(res->mInf1Ptr->defaultCode);
}

uint32_t nextCodePoint(const std::string& s, size_t& i) {
    const auto b = static_cast<uint8_t>(s[i++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = b & (0x3F >> extra);
    for (; extra > 0; extra--) {
        if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = cp << 6 | (static_cast<uint8_t>(s[i++]) & 0x3F);
    }
    return cp;
}

// ASCII as is; kana on a Shift-JIS font; CP1252 letters otherwise; '?' for what cannot draw.
std::string fontGlyph(JUTFont* font, uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) {
        return std::string(1, static_cast<char>(cp));
    }
    const bool sjis = font->getFontType() == 2;
    int code = -1;
    if (sjis) {
        if (cp >= 0x3041 && cp <= 0x3093) {
            code = 0x829F + (cp - 0x3041);
        } else if (cp >= 0x30A1 && cp <= 0x30F6) {
            const int n = static_cast<int>(cp - 0x30A1);
            code = 0x8340 + n + (n >= 0x3F ? 1 : 0);  // 0x837F is not a kana
        } else if (cp == 0x30FC) {
            code = 0x815B;
        }
        if (code > 0 && fontHasGlyph(font, code)) {
            return {static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
        }
    } else {
        if (cp >= 0xA0 && cp <= 0xFF) {
            code = static_cast<int>(cp);
        } else if (cp == 0x0152) {
            code = 0x8C;
        } else if (cp == 0x0153) {
            code = 0x9C;
        }
        if (code > 0 && fontHasGlyph(font, code)) {
            return std::string(1, static_cast<char>(code));
        }
    }
    return "?";
}

// At most `maxGlyphs` glyphs; a longer text is cut to leave room for "...".
std::string tagText(JUTFont* font, const std::string& utf8, size_t maxGlyphs) {
    std::vector<std::string> glyphs;
    for (size_t i = 0; i < utf8.size();) {
        glyphs.push_back(fontGlyph(font, nextCodePoint(utf8, i)));
    }
    if (glyphs.size() > maxGlyphs) {
        glyphs.resize(maxGlyphs - 3);
        glyphs.insert(glyphs.end(), 3, ".");
    }
    std::string out;
    for (const std::string& g : glyphs) out += g;
    return out;
}

std::string ownerText(JUTFont* font, uint32_t clientId, const std::string& name) {
    return tagText(font, name.empty() ? "#" + std::to_string(clientId) : name, kMaxOwnerGlyphs);
}

std::string cardText(JUTFont* font, uint32_t clientId, const Client& c,
    const horse::TagInfo* horse) {
    if (c.self || horse == nullptr || horse->ridden) {
        return {};
    }
    return ownerText(font, clientId, c.name) + "'s " +
           tagText(font, horse::remoteName(c), kMaxHorseGlyphs);
}

std::string riderLine(JUTFont* font, const Client& c, const horse::TagInfo* horse) {
    if (horse == nullptr || !horse->ridden) {
        return {};
    }
    return tagText(font, horse::remoteName(c), kMaxHorseGlyphs);
}

const horse::TagInfo* findHorse(uint32_t clientId, horse::TagInfo& storage) {
    return horse::tagInfo(clientId, storage) ? &storage : nullptr;
}

JUTFont* tagFont() {
    JUTFont* font = mDoExt_getSubFont();
    if (font == nullptr || !font->isValid()) {
        font = mDoExt_getMesgFont();
    }
    return font != nullptr && font->isValid() ? font : nullptr;
}

bool hasUsableAttentionPoint(const fopAc_ac_c* actor) {
    const cXyz& pos = actor->current.pos;
    const cXyz& attention = actor->attention_info.position;
    const f32 dx = attention.x - pos.x;
    const f32 dz = attention.z - pos.z;
    return std::isfinite(attention.x) && std::isfinite(attention.y) &&
           std::isfinite(attention.z) && std::fabs(dx) < 300.0f && std::fabs(dz) < 300.0f &&
           attention.y > pos.y - 80.0f;
}

f32 measureWidth(JUTFont* font, const std::string& text, f32 glyphWidth) {
    if (font == nullptr || text.empty()) {
        return 0.0f;
    }
    const f32 scale = glyphWidth / static_cast<f32>(font->getCellWidth());
    f32 width = 0.0f;
    for (size_t i = 0; i < text.size(); i++) {
        int code = static_cast<unsigned char>(text[i]);
        // A Shift-JIS glyph is two bytes.
        if (font->isLeadByte(code) && i + 1 < text.size()) {
            code = code << 8 | static_cast<unsigned char>(text[++i]);
        }
        width += static_cast<f32>(font->getWidth(code)) * scale;
    }
    return width;
}

// False when `world` is behind the camera or far off screen.
bool projectTag(const Vec& world, f32 minX, f32 minY, f32 maxX, f32 maxY, Vec& screen) {
    Vec camera = {};
    mDoLib_pos2camera(const_cast<Vec*>(&world), &camera);
    if (camera.z >= -10.0f) {
        return false;
    }
    mDoLib_project(const_cast<Vec*>(&world), &screen);
    return screen.x >= minX - 160.0f && screen.x <= maxX + 160.0f && screen.y >= minY - 80.0f &&
           screen.y <= maxY + 80.0f;
}

// 1 up to 1200 units from our player, fading out to 0 at 1600; -1 beyond.
f32 tagAlpha(const fopAc_ac_c* player, const cXyz& pos, f32& distanceSq) {
    distanceSq = 0.0f;
    if (player == nullptr) {
        return 1.0f;
    }
    distanceSq = player->current.pos.abs2(pos);
    constexpr f32 kFadeStartDistanceSq = 1440000.0f;
    constexpr f32 kMaxDistanceSq = 2560000.0f;
    if (distanceSq > kMaxDistanceSq) {
        return -1.0f;
    }
    if (distanceSq <= kFadeStartDistanceSq) {
        return 1.0f;
    }
    return std::clamp(
        (kMaxDistanceSq - distanceSq) / (kMaxDistanceSq - kFadeStartDistanceSq), 0.0f, 1.0f);
}

void layoutTag(JUTFont* font, TagDraw& tag, const Vec& screen, f32 minX, f32 maxX) {
    f32 textWidth = 0.0f;
    f32 textHeight = 0.0f;
    for (int i = 0; i < tag.lineCount; i++) {
        TagLine& line = tag.lines[i];
        line.width = measureWidth(font, line.text, line.glyphW);
        textWidth = std::max(textWidth, line.width);
        textHeight += line.glyphH + (i > 0 ? kLineGap : 0.0f);
    }
    tag.width = textWidth + kPadX * 2.0f + kDotRoom;
    tag.height = textHeight + kPadY * 2.0f;
    tag.x = std::clamp(screen.x - tag.width * 0.5f, minX + 4.0f, maxX - tag.width - 4.0f);
    tag.y = screen.y - tag.height - 8.0f;
}

JUtility::TColor fadedColor(u8 r, u8 g, u8 b, u8 a, f32 alpha) {
    const int faded = std::clamp(static_cast<int>(static_cast<f32>(a) * alpha), 0, 255);
    return JUtility::TColor(r, g, b, static_cast<u8>(faded));
}

void drawTagBox(const TagDraw& tag) {
    J2DFillBox(tag.x, tag.y, tag.width, tag.height, tag.fill);
    J2DDrawFrame(tag.x, tag.y, tag.width, tag.height, tag.border, 2);
    // The owner's dot, level with the name.
    J2DFillBox(tag.x + kDotX, tag.y + kPadY + tag.lines[0].glyphH * 0.5f - kDotSize * 0.5f,
        kDotSize, kDotSize, tag.border);
}

void addProbe(uint32_t id, Probe::Kind kind, const TagDraw& tag) {
    s_drawn.push_back({id, kind, tag.lines[0].text, tag.lineCount > 1 ? tag.lines[1].text : "",
        tag.x, tag.y, tag.width, tag.height, tag.border.r, tag.border.g, tag.border.b});
}

void draw() {
    ++s_frames;
    s_drawn.clear();
    if (!Session::active()) {
        s_gate = "inactive";
        return;
    }
    const Session& session = Session::instance();
    // Under the HUD a menu would cover them anyway; its captured frame shows through otherwise.
    if (!session.isConnected()) {
        s_gate = "notConnected";
        return;
    }
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0) {
        s_gate = "menu";
        return;
    }
    if (localCutsceneRunning() || hideRemotePlayersForCutscene()) {
        s_gate = "cutscene";
        return;
    }
    const char* myStage = dComIfGp_getStartStageName();
    if (myStage == nullptr || dComIfGd_getView() == nullptr || dComIfGd_getViewport() == nullptr) {
        s_gate = "noStage";
        return;
    }
    JUTFont* font = tagFont();
    if (font == nullptr) {
        s_gate = "noFont";
        return;
    }
    s_gate = "";

    const int8_t myLayer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const f32 minX = mDoGph_gInf_c::getMinXF();
    const f32 minY = mDoGph_gInf_c::getMinYF();
    const f32 maxX = minX + mDoGph_gInf_c::getWidthF();
    const f32 maxY = minY + mDoGph_gInf_c::getHeightF();

    std::vector<TagDraw> tags;
    // A card over each unridden remote horse in sight ("Dad's Lilly").
    for (const auto& [id, client] : session.clients()) {
        horse::TagInfo storage;
        const horse::TagInfo* horse = findHorse(id, storage);
        std::string text = cardText(font, id, client, horse);
        if (text.empty()) {
            continue;
        }
        const cXyz pos = presented((uint64_t{1} << 32) | id, cXyz(horse->pos[0], horse->pos[1],
                                                                horse->pos[2]));
        f32 distanceSq = 0.0f;
        const f32 alpha = tagAlpha(player, pos, distanceSq);
        if (alpha < 0.0f) {
            continue;
        }
        const Vec world = {pos.x + horse->eye[0] - horse->pos[0],
            pos.y + horse->eye[1] - horse->pos[1] + 70.0f, pos.z + horse->eye[2] - horse->pos[2]};
        Vec screen = {};
        if (!projectTag(world, minX, minY, maxX, maxY, screen)) {
            continue;
        }
        TagDraw card;
        card.lines[0] = {std::move(text), kSmallGlyphWidth, kSmallGlyphHeight};
        card.distanceSq = distanceSq;
        card.fill = fadedColor(10, 12, 16, 170, alpha);
        card.border = fadedColor(client.colorR, client.colorG, client.colorB, 220, alpha);
        card.text = fadedColor(245, 247, 250, 240, alpha);
        card.text2 = card.text;
        layoutTag(font, card, screen, minX, maxX);
        addProbe(id, Probe::Kind::HorseCard, card);
        tags.push_back(std::move(card));
    }

    for (const auto& [id, client] : session.clients()) {
        if (client.self || !client.online || !client.isSaveLoaded || !client.hasPlayerUpdate ||
            hideRemoteClientForCutscene(client))
        {
            continue;
        }
        if (std::strncmp(client.stageName, myStage, sizeof(client.stageName)) != 0 ||
            client.layerNo != myLayer)
        {
            continue;
        }
        fopAc_ac_c* actor = session.dummyActorForClient(id);
        if (actor == nullptr || (actor->actor_status & fopAcStts_NODRAW_e) ||
            !IsDummyPlayerShown(actor))
        {
            continue;
        }
        cXyz anchor(actor->current.pos.x, actor->current.pos.y + 220.0f, actor->current.pos.z);
        if (hasUsableAttentionPoint(actor)) {
            anchor.x = actor->attention_info.position.x;
            anchor.y = std::max(anchor.y, actor->attention_info.position.y + 24.0f);
            anchor.z = actor->attention_info.position.z;
        }
        const cXyz world = presented(id, anchor);
        f32 distanceSq = 0.0f;
        const f32 alpha = tagAlpha(player, world, distanceSq);
        if (alpha < 0.0f) {
            continue;
        }
        Vec screen = {};
        if (!projectTag(world, minX, minY, maxX, maxY, screen)) {
            continue;
        }
        TagDraw tag;
        tag.lines[0] = {ownerText(font, id, client.name), kGlyphWidth, kGlyphHeight};
        horse::TagInfo storage;
        if (std::string line2 = riderLine(font, client, findHorse(id, storage)); !line2.empty()) {
            tag.lines[1] = {std::move(line2), kSmallGlyphWidth, kSmallGlyphHeight};
            tag.lineCount = 2;
        }
        tag.distanceSq = distanceSq;
        tag.fill = fadedColor(10, 12, 16, 190, alpha);
        tag.border = fadedColor(client.colorR, client.colorG, client.colorB, 240, alpha);
        tag.text = fadedColor(245, 247, 250, 245, alpha);
        tag.text2 = fadedColor(200, 206, 214, 240, alpha);
        layoutTag(font, tag, screen, minX, maxX);
        addProbe(id, Probe::Kind::Player, tag);
        tags.push_back(std::move(tag));
    }
    if (tags.empty()) {
        return;
    }

    std::sort(tags.begin(), tags.end(),
        [](const TagDraw& a, const TagDraw& b) { return a.distanceSq > b.distanceSq; });
    for (const TagDraw& tag : tags) {
        drawTagBox(tag);
    }
    font->setGX();
    for (const TagDraw& tag : tags) {
        f32 baseline = tag.y + kPadY;
        for (int i = 0; i < tag.lineCount; i++) {
            const TagLine& line = tag.lines[i];
            baseline += line.glyphH + (i > 0 ? kLineGap : 0.0f);
            font->setCharColor(i == 0 ? tag.text : tag.text2);
            // drawString_scale's y is the baseline, a quarter glyph above the cell's bottom.
            font->drawString_scale(tag.x + kDotRoom + (tag.width - kDotRoom - line.width) * 0.5f,
                baseline - line.glyphH * 0.25f, line.glyphW, line.glyphH, line.text.c_str(), true);
        }
    }
    // Leave the 2D port as the HUD expects it.
    dComIfGp_getCurrentGrafPort()->setPort();
}

void onFrameBeforeHud(ModContext*, const GfxStageContext*, void*) {
    draw();
}

}  // namespace

bool install() {
    if (svc_gfx == nullptr) {
        TwiliLog.warn("[ui] no GfxService: name tags are off");
        return false;
    }
    GfxStageHookDesc desc = GFX_STAGE_HOOK_DESC_INIT;
    desc.callback = onFrameBeforeHud;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &desc, &s_hook) !=
        MOD_OK)
    {
        TwiliLog.warn("[ui] could not register the name tag stage hook");
        s_hook = 0;
        return false;
    }
    return true;
}

void uninstall() {
    s_hook = 0;
    s_drawn.clear();
    s_tracks.clear();
}

const std::vector<Probe>& drawn() {
    return s_drawn;
}

uint32_t frameCount() {
    return s_frames;
}

const char* lastGate() {
    return s_gate;
}

std::string horseCardText(uint32_t clientId) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(clientId);
    JUTFont* font = tagFont();
    if (it == clients.end() || font == nullptr) {
        return {};
    }
    horse::TagInfo storage;
    return cardText(font, clientId, it->second, findHorse(clientId, storage));
}

std::string riderTagLine2(uint32_t clientId) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(clientId);
    JUTFont* font = tagFont();
    if (it == clients.end() || font == nullptr) {
        return {};
    }
    horse::TagInfo storage;
    return riderLine(font, it->second, findHorse(clientId, storage));
}

}  // namespace twili::ui::name_tags
