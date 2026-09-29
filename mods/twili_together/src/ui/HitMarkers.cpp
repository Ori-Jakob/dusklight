#include "ui/HitMarkers.hpp"

#include "core/Host.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"

#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JUtility/TColor.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_lib.h"

#include <mods/svc/gfx.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace twili::ui::hit_markers {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kLifeSec = 0.9f;
constexpr float kRise = 60.0f;
constexpr float kGlyphWidth = 18.0f;
constexpr float kGlyphHeight = 22.0f;
constexpr float kMaxDistanceSq = 4000.0f * 4000.0f;
constexpr size_t kMaxMarkers = 8;

struct Marker {
    cXyz pos;
    std::string text;
    JUtility::TColor color;
    Clock::time_point born;
};

GfxStageHookHandle s_hook = 0;
std::vector<Marker> s_markers;
uint32_t s_drawnFrames = 0;

JUTFont* markerFont() {
    JUTFont* font = mDoExt_getSubFont();
    if (font == nullptr || !font->isValid()) {
        font = mDoExt_getMesgFont();
    }
    return font != nullptr && font->isValid() ? font : nullptr;
}

float textWidth(JUTFont* font, const std::string& text, float glyphWidth) {
    const float scale = glyphWidth / static_cast<float>(font->getCellWidth());
    float width = 0.0f;
    for (const char c : text) {
        width += static_cast<float>(font->getWidth(static_cast<unsigned char>(c))) * scale;
    }
    return width;
}

bool drawAllowed() {
    if (!Session::active() || !Session::instance().isConnected()) {
        return false;
    }
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0 || localCutsceneRunning()) {
        return false;
    }
    return dComIfGp_getStartStageName() != nullptr && dComIfGd_getView() != nullptr &&
           dComIfGd_getViewport() != nullptr;
}

void draw() {
    const auto now = Clock::now();
    const auto age = [&](const Marker& m) {
        return std::chrono::duration<float>(now - m.born).count();
    };
    s_markers.erase(std::remove_if(s_markers.begin(), s_markers.end(),
                        [&](const Marker& m) { return age(m) >= kLifeSec; }),
        s_markers.end());
    if (s_markers.empty() || !drawAllowed()) {
        return;
    }
    JUTFont* font = markerFont();
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (font == nullptr) {
        return;
    }
    dComIfGp_getCurrentGrafPort()->setPort();
    font->setGX();
    bool drew = false;
    for (const Marker& m : s_markers) {
        if (player != nullptr && player->current.pos.abs2(m.pos) > kMaxDistanceSq) {
            continue;
        }
        const float t = age(m) / kLifeSec;
        // Eases up, pops in slightly larger, fades over its last 40%.
        Vec world = {m.pos.x, m.pos.y + kRise * (1.0f - (1.0f - t) * (1.0f - t)), m.pos.z};
        Vec camera = {};
        mDoLib_pos2camera(&world, &camera);
        if (camera.z >= -10.0f) {
            continue;
        }
        Vec screen = {};
        mDoLib_project(&world, &screen);
        const float alpha = t < 0.6f ? 1.0f : std::max(0.0f, 1.0f - (t - 0.6f) / 0.4f);
        const float pop = t < 0.12f ? 1.3f - 0.3f * (t / 0.12f) : 1.0f;
        const float w = kGlyphWidth * pop;
        const float h = kGlyphHeight * pop;
        const float x = screen.x - textWidth(font, m.text, w) * 0.5f;
        const u8 a = static_cast<u8>(std::clamp(alpha * 255.0f, 0.0f, 255.0f));
        // A dark outline keeps it readable on grass and sky alike.
        font->setCharColor(JUtility::TColor(0, 0, 0, static_cast<u8>(a * 0.85f)));
        for (const float d : {-1.5f, 1.5f}) {
            font->drawString_scale(x + d, screen.y + d, w, h, m.text.c_str(), true);
            font->drawString_scale(x + d, screen.y - d, w, h, m.text.c_str(), true);
        }
        font->setCharColor(JUtility::TColor(m.color.r, m.color.g, m.color.b, a));
        font->drawString_scale(x, screen.y, w, h, m.text.c_str(), true);
        drew = true;
    }
    if (drew) {
        s_drawnFrames++;
    }
    dComIfGp_getCurrentGrafPort()->setPort();
}

void onFrameBeforeHud(ModContext*, const GfxStageContext*, void*) {
    draw();
}

}  // namespace

bool install() {
    if (svc_gfx == nullptr) {
        return false;
    }
    GfxStageHookDesc desc = GFX_STAGE_HOOK_DESC_INIT;
    desc.callback = onFrameBeforeHud;
    if (svc_gfx->register_stage_hook(mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &desc, &s_hook) != MOD_OK)
    {
        TwiliLog.warn("[ui] could not register the PvP hit marker stage hook");
        s_hook = 0;
        return false;
    }
    return true;
}

void uninstall() {
    s_hook = 0;
    s_markers.clear();
}

std::string text(bool blocked, int damage) {
    if (blocked) {
        return "Blocked";
    }
    if (damage <= 0) {
        return "Hit";
    }
    // Quarter hearts as hearts: 2 is "-1/2", 5 is "-1 1/4".
    static constexpr const char* kQuarters[] = {"", "1/4", "1/2", "3/4"};
    const int whole = damage / 4;
    const int rest = damage % 4;
    if (whole == 0) {
        return fmt::format("-{}", kQuarters[rest]);
    }
    return rest == 0 ? fmt::format("-{}", whole) : fmt::format("-{} {}", whole, kQuarters[rest]);
}

void spawn(uint32_t clientId, const float offset[3], bool blocked, int damage) {
    const fopAc_ac_c* dummy = Session::instance().dummyActorForClient(clientId);
    if (dummy == nullptr) {
        return;
    }
    Marker m;
    m.pos.set(dummy->current.pos.x + offset[0], dummy->current.pos.y + offset[1] + 20.0f,
        dummy->current.pos.z + offset[2]);
    m.text = text(blocked, damage);
    m.color = blocked    ? JUtility::TColor(180, 215, 255, 255) :
              damage > 0 ? JUtility::TColor(255, 120, 90, 255) :
                           JUtility::TColor(255, 230, 150, 255);
    m.born = Clock::now();
    if (s_markers.size() >= kMaxMarkers) {
        s_markers.erase(s_markers.begin());
    }
    s_markers.push_back(std::move(m));
}

uint32_t drawnFrames() {
    return s_drawnFrames;
}

}  // namespace twili::ui::hit_markers
