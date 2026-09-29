#include "hooks/Hooks.hpp"

#include "core/Config.hpp"
#include "ui/MapCursors.hpp"

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

namespace {

// dMeterMap_c::draw's picture rect: the user HUD scale keeps the bottom-left corner anchored.
void onMeterMapDrawPost(ModContext*, void* args, void*, void*) {
    auto* meter = mods::arg<dMeterMap_c*>(args, 0);
    if (meter == nullptr || meter->mMap == nullptr || !meter->mMap->isDraw()) {
        return;
    }
    const f32 scale =
        static_cast<f32>(std::clamp(config::hostFloat("game.hudScale", 1.0), 0.5, 2.0));
    const f32 w = meter->mSizeW * scale;
    const f32 h = meter->mSizeH * scale;
    ui::map_cursor::drawMinimap(*meter->mMap, mDoGph_gInf_c::ScaleHUDXLeft(meter->mDrawPosX),
        meter->mDrawPosY + (meter->mSizeH - h), w, h, meter->mMapAlpha);
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
    const ModResult results[] = {
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
