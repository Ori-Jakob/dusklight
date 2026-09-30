#include "hooks/Hooks.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "ui/MapCursors.hpp"

#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/d_map.h"
#include "d/d_menu_dmap.h"
#include "d/d_menu_dmap_map.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "d/d_menu_map_common.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter_map.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>
#include <cmath>

namespace twili::hooks {

DEFINE_HOOK(&dMeterMap_c::draw, MeterMapDraw);
// dMenu_DmapBg_c has two bases: MSVC cannot constant-initialize a DEFINE_HOOK record for it.
DEFINE_HOOK_SYMBOL("dMenu_DmapBg_c::draw", void(dMenu_DmapBg_c*), DmapBgDraw);
static_assert(std::is_same_v<decltype(&dMenu_DmapBg_c::draw), void (dMenu_DmapBg_c::*)()>);
DEFINE_HOOK(&dMenuMapCommon_c::drawIcon, MapCommonDrawIcon);
DEFINE_HOOK(&dMenu_Dmap_c::getIconPos, DmapGetIconPos);
// The same for the field map's background screen.
DEFINE_HOOK_SYMBOL("dMenu_Fmap2DBack_c::draw", void(dMenu_Fmap2DBack_c*), FmapBackDraw);
static_assert(std::is_same_v<decltype(&dMenu_Fmap2DBack_c::draw), void (dMenu_Fmap2DBack_c::*)()>);
DEFINE_HOOK(static_cast<void (dMenu_Fmap_c::*)(f32, bool)>(&dMenu_Fmap_c::drawIcon), FmapDrawIcon);
DEFINE_HOOK(
    static_cast<void (J2DPicture::*)(f32, f32, f32, f32, bool, bool, bool)>(&J2DPicture::draw),
    PictureDraw);

namespace {

struct Rect {
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// The minimap picture as this dMeterMap_c::draw call drew it.
const J2DPicture* s_minimapPicture = nullptr;
bool s_minimapCaptured = false;
Rect s_minimapRect;
bool s_pictureHook = false;
Rect s_loggedFormula;

HookAction onMeterMapDrawPre(ModContext*, void* args, void*, void*) {
    auto* meter = mods::arg<dMeterMap_c*>(args, 0);
    s_minimapPicture = meter != nullptr ? meter->mMapJ2DPicture : nullptr;
    s_minimapCaptured = false;
    return HOOK_CONTINUE;
}

// The quad (0,0)-(w,h) through the matrix the picture loaded for it.
void onPictureDrawPost(ModContext*, void* args, void*, void*) {
    auto* picture = mods::arg<J2DPicture*>(args, 0);
    if (picture == nullptr || picture != s_minimapPicture || !picture->isVisible()) {
        return;
    }
    const f32 w = mods::arg<f32>(args, 3);
    const f32 h = mods::arg<f32>(args, 4);
    const Mtx& m = *picture->getMtx();
    const f32 x1 = m[0][0] * w + m[0][1] * h + m[0][3];
    const f32 y1 = m[1][0] * w + m[1][1] * h + m[1][3];
    s_minimapRect = {std::min(m[0][3], x1), std::min(m[1][3], y1), std::fabs(x1 - m[0][3]),
        std::fabs(y1 - m[1][3])};
    s_minimapCaptured = true;
}

// dMeterMap_c::draw's own rect: the HUD scale keeps the bottom-left corner anchored.
Rect formulaRect(const dMeterMap_c& meter) {
    const f32 scale =
        static_cast<f32>(std::clamp(config::hostFloat("game.hudScale", 1.0), 0.5, 2.0));
    const f32 w = meter.mSizeW * scale;
    const f32 h = meter.mSizeH * scale;
    return {
        mDoGph_gInf_c::ScaleHUDXLeft(meter.mDrawPosX), meter.mDrawPosY + (meter.mSizeH - h), w, h};
}

bool sameRect(const Rect& a, const Rect& b) {
    return std::fabs(a.x - b.x) < 0.5f && std::fabs(a.y - b.y) < 0.5f &&
           std::fabs(a.w - b.w) < 0.5f && std::fabs(a.h - b.h) < 0.5f;
}

// Markers go where the picture actually went; the formula is the fallback.
void onMeterMapDrawPost(ModContext*, void* args, void*, void*) {
    auto* meter = mods::arg<dMeterMap_c*>(args, 0);
    s_minimapPicture = nullptr;
    if (meter == nullptr || meter->mMap == nullptr || !meter->mMap->isDraw()) {
        return;
    }
    Rect r = formulaRect(*meter);
    if (s_minimapCaptured) {
        if (!sameRect(r, s_minimapRect) && !sameRect(r, s_loggedFormula)) {
            s_loggedFormula = r;
            TwiliLog.warn("[map] minimap drawn at ({:.1f}, {:.1f}) {:.1f}x{:.1f}, formula said "
                          "({:.1f}, {:.1f}) {:.1f}x{:.1f}",
                s_minimapRect.x, s_minimapRect.y, s_minimapRect.w, s_minimapRect.h, r.x, r.y, r.w,
                r.h);
        }
        r = s_minimapRect;
    } else if (s_pictureHook) {
        return;  // the picture did not draw
    }
    ui::map_cursor::drawMinimap(
        *meter->mMap, r.x, r.y, r.w, r.h, meter->mMapAlpha, s_minimapCaptured);
}

HookAction onDmapBgDrawPre(ModContext*, void* args, void*, void*) {
    Scope::push(ScopeKind::DmapDraw, mods::arg<dMenu_DmapBg_c*>(args, 0));
    return HOOK_CONTINUE;
}

void onDmapBgDrawPost(ModContext*, void* args, void*, void*) {
    auto* bg = mods::arg<dMenu_DmapBg_c*>(args, 0);
    // A higher-priority pre-hook that skipped the draw also skipped our push.
    if (Scope::owner(ScopeKind::DmapDraw) == bg) {
        Scope::pop(ScopeKind::DmapDraw, bg);
    }
}

HookAction onFmapBackDrawPre(ModContext*, void* args, void*, void*) {
    Scope::push(ScopeKind::FmapDraw, mods::arg<dMenu_Fmap2DBack_c*>(args, 0));
    return HOOK_CONTINUE;
}

void onFmapBackDrawPost(ModContext*, void* args, void*, void*) {
    auto* back = mods::arg<dMenu_Fmap2DBack_c*>(args, 0);
    if (Scope::owner(ScopeKind::FmapDraw) == back) {
        Scope::pop(ScopeKind::FmapDraw, back);
    }
}

// The call right after the map pane: remote players go under the icons, so Link stays on top.
HookAction onDrawIconPre(ModContext*, void* args, void*, void*) {
    auto* common = mods::arg<dMenuMapCommon_c*>(args, 0);
    const f32 x = mods::arg<f32>(args, 1);
    const f32 y = mods::arg<f32>(args, 2);
    const auto* bg = static_cast<const dMenu_DmapBg_c*>(Scope::owner(ScopeKind::DmapDraw));
    if (bg != nullptr && common == static_cast<const dMenuMapCommon_c*>(bg)) {
        ui::map_cursor::drawPauseDmap(*common, x, y, mods::arg<f32>(args, 3));
        return HOOK_CONTINUE;
    }
    const auto* back = static_cast<const dMenu_Fmap2DBack_c*>(Scope::owner(ScopeKind::FmapDraw));
    if (back != nullptr && common == static_cast<const dMenuMapCommon_c*>(back)) {
        // Its pictures fade with both of drawIcon's alphas.
        ui::map_cursor::drawPauseFmap(
            *common, x, y, mods::arg<f32>(args, 3) * mods::arg<f32>(args, 4));
    }
    return HOOK_CONTINUE;
}

// The Link icon is placed: remote players the same way, in the stage name it used.
void onFmapDrawIconPost(ModContext*, void* args, void*, void*) {
    auto* fmap = mods::arg<dMenu_Fmap_c*>(args, 0);
    if (fmap == nullptr || fmap->mpDraw2DBack == nullptr) {
        return;
    }
    const char* stage = dComIfGs_isPlayerFieldLastStayFieldDataExistFlag()
                            ? dMenuFmap_getStartStageName(fmap->mpFieldDat)
                            : nullptr;
    ui::map_cursor::collectPauseFmap(*fmap->mpDraw2DBack, stage);
}

void cnvPauseDmap(const void* ctx, f32 x, f32 z, f32* px, f32* py) {
    static_cast<const dMenu_DmapMapCtrl_c*>(ctx)->cnvPosTo2Dpos(x, z, px, py);
}

// Remote players on this floor, placed like the Link icon.
void onGetIconPosPost(ModContext*, void* args, void*, void*) {
    auto* dmap = mods::arg<dMenu_Dmap_c*>(args, 0);
    if (dmap == nullptr || dmap->mMapCtrl == nullptr) {
        return;
    }
    ui::map_cursor::collectPauseDmap(
        mods::arg<s8>(args, 1), mods::arg<f32>(args, 2), cnvPauseDmap, dmap->mMapCtrl);
}

}  // namespace

ModResult installMap(std::string& error) {
    // Without it the markers use the formula.
    std::string pictureError;
    s_pictureHook = addPost<PictureDraw>(
                        onPictureDrawPost, kDefault, "J2DPicture::draw", pictureError) == MOD_OK;
    if (!s_pictureHook) {
        TwiliLog.warn("[map] {}", pictureError);
    }
    const ModResult results[] = {
        addPre<MeterMapDraw>(onMeterMapDrawPre, kObserve, "dMeterMap_c::draw", error),
        addPost<MeterMapDraw>(onMeterMapDrawPost, kDefault, "dMeterMap_c::draw", error),
        addPre<DmapBgDraw>(onDmapBgDrawPre, kObserve, "dMenu_DmapBg_c::draw", error),
        addPost<DmapBgDraw>(onDmapBgDrawPost, kObserve, "dMenu_DmapBg_c::draw", error),
        addPre<MapCommonDrawIcon>(onDrawIconPre, kDefault, "dMenuMapCommon_c::drawIcon", error),
        addPost<DmapGetIconPos>(onGetIconPosPost, kDefault, "dMenu_Dmap_c::getIconPos", error),
        addPre<FmapBackDraw>(onFmapBackDrawPre, kObserve, "dMenu_Fmap2DBack_c::draw", error),
        addPost<FmapBackDraw>(onFmapBackDrawPost, kObserve, "dMenu_Fmap2DBack_c::draw", error),
        addPost<FmapDrawIcon>(onFmapDrawIconPost, kDefault, "dMenu_Fmap_c::drawIcon", error),
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
