#include "hooks.hpp"

#include "JSystem/J3DGraphBase/J3DSys.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_tev_str.h"
#include "dolphin/gx/GXLighting.h"
#include "m_Do/m_Do_mtx.h"
#include "mods/hook.hpp"
#include "mods/service.hpp"

#include <algorithm>

namespace lighting::hooks {
namespace {

Options g_options;

DEFINE_HOOK(&dScnKy_env_light_c::settingTevStruct_plightcol_plus, PointLightTevBoost);
DEFINE_HOOK(&dKy_setLight_nowroom_actor, ActorRoomLights);
DEFINE_HOOK_SYMBOL("dKy_GlobalLight_set", void(), GlobalLightLoader);
DEFINE_HOOK(&dKy_setLight_again, ReloadGlobalLights);

bool get_bool(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    return g_options.config != nullptr && handle != 0 &&
                   g_options.config->get_bool(mod_ctx, handle, &value) == MOD_OK ?
               value :
               fallback;
}

int64_t get_int(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    return g_options.config != nullptr && handle != 0 &&
                   g_options.config->get_int(mod_ctx, handle, &value) == MOD_OK ?
               value :
               fallback;
}

int suppression_level() {
    if (!get_bool(g_options.enabled, false))
        return 0;
    return static_cast<int>(std::clamp<int64_t>(get_int(g_options.suppression_level, 0), 0, 2));
}

void zero_light_color(J3DLightInfo* light) {
    if (light == nullptr)
        return;
    light->mColor.r = 0;
    light->mColor.g = 0;
    light->mColor.b = 0;
}

GXColor base_light_color(const dScnKy_env_light_c& env) {
    const auto channel = [](s16 value) { return static_cast<u8>(std::clamp<int>(value, 0, 255)); };
    return {channel(env.base_light.mColor.r), channel(env.base_light.mColor.g),
        channel(env.base_light.mColor.b), 0xFF};
}

void initialize_light(
    GXLightObj& light, const Vec& position, GXColor color, GXDistAttnFn distance_function) {
    GXInitLightPos(&light, position.x, position.y, position.z);
    GXInitLightDir(&light, 0.0f, -1.0f, 0.0f);
    GXInitLightColor(&light, color);
    GXInitLightSpot(&light, 90.0f, GX_SP_OFF);
    GXInitLightDistAttn(
        &light, distance_function == GX_DA_OFF ? 1.0f : 10000.0f, 0.99999f, distance_function);
}

void load_suppressed_global_lights() {
    static constexpr GXLightID kIds[] = {
        GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3, GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7};
    const GXColor black{0, 0, 0, 0xFF};
    const Vec origin{0.0f, 0.0f, 0.0f};
    GXLightObj light{};
    initialize_light(light, origin, black, GX_DA_OFF);
    for (GXLightID id : kIds)
        GXLoadLightObjImm(&light, id);

    if (!get_bool(g_options.keep_gx_sun, true) || dKy_SunMoon_Light_Check() != TRUE)
        return;
    dScnKy_env_light_c* env = dKy_getEnvlight();
    MtxP view = j3dSys.getViewMtx();
    if (env == nullptr || view == nullptr)
        return;

    Vec position_view{};
    cMtx_multVec(view, &env->sun_pos, &position_view);
    initialize_light(light, position_view, base_light_color(*env), GX_DA_STEEP);
    // Vanilla reserves both slots for the current sun/moon directional pair. They intentionally
    // share the far-away position here, matching the global loader's two-slot sun path.
    GXLoadLightObjImm(&light, GX_LIGHT2);
    GXLoadLightObjImm(&light, GX_LIGHT3);
}

HookAction on_point_light_tev_pre(ModContext*, void* args, void*, void*) {
    if (suppression_level() < 1)
        return HOOK_CONTINUE;
    dKy_tevstr_c* tev = mods::arg<dKy_tevstr_c*>(args, 2);
    if (tev != nullptr)
        zero_light_color(tev->mLightObj.getLightInfo());
    return HOOK_SKIP_ORIGINAL;
}

void on_actor_room_lights_post(ModContext*, void* args, void*, void*) {
    if (suppression_level() < 1)
        return;
    dKy_tevstr_c* tev = mods::arg<dKy_tevstr_c*>(args, 0);
    if (tev == nullptr)
        return;
    for (J3DLightObj& light : tev->mLights)
        zero_light_color(light.getLightInfo());
}

HookAction on_global_light_pre(ModContext*, void*, void*, void*) {
    if (suppression_level() < 2)
        return HOOK_CONTINUE;
    load_suppressed_global_lights();
    return HOOK_SKIP_ORIGINAL;
}

}  // namespace

ModResult initialize(const HookService* service, const Options& options, ModError* error) {
    g_options = options;
    if (mods::hook_add_pre<PointLightTevBoost>(service, on_point_light_tev_pre) != MOD_OK ||
        mods::hook_add_post<ActorRoomLights>(service, on_actor_room_lights_post) != MOD_OK ||
        mods::hook_add_pre<GlobalLightLoader>(service, on_global_light_pre) != MOD_OK ||
        mods::hook_add_pre<ReloadGlobalLights>(service, on_global_light_pre) != MOD_OK)
    {
        g_options = {};
        return mods::set_error(
            error, MOD_ERROR, "failed to install native-light suppression hooks");
    }
    return MOD_OK;
}

void shutdown() {
    g_options = {};
}

}  // namespace lighting::hooks
