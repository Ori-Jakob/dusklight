#include "hooks/Hooks.hpp"

#include "core/Config.hpp"
#include "ui/MapCursors.hpp"

#include "d/d_map.h"
#include "d/d_menu_dmap.h"
#include "d/d_menu_dmap_map.h"
#include "d/d_menu_map_common.h"
#include "d/d_meter_map.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>

namespace twili::hooks {

DEFINE_HOOK(&dMeterMap_c::draw, MeterMapDraw);
DEFINE_HOOK(&dMenu_DmapBg_c::draw, DmapBgDraw);
DEFINE_HOOK(&dMenuMapCommon_c::drawIcon, MapCommonDrawIcon);
DEFINE_HOOK(&dMenu_Dmap_c::getIconPos, DmapGetIconPos);

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

// The call right after the map pane: remote players go under the icons, so Link stays on top.
HookAction onDrawIconPre(ModContext*, void* args, void*, void*) {
    const auto* bg = static_cast<const dMenu_DmapBg_c*>(Scope::owner(ScopeKind::DmapDraw));
    auto* common = mods::arg<dMenuMapCommon_c*>(args, 0);
    if (bg == nullptr || common != static_cast<const dMenuMapCommon_c*>(bg)) {
        return HOOK_CONTINUE;
    }
    ui::map_cursor::drawPauseDmap(
        *common, mods::arg<f32>(args, 1), mods::arg<f32>(args, 2), mods::arg<f32>(args, 3));
    return HOOK_CONTINUE;
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
    };
    for (const ModResult r : results) {
        if (r != MOD_OK) {
            return r;
        }
    }
    return MOD_OK;
}

}  // namespace twili::hooks
