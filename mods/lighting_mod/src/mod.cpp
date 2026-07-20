#include "common.hpp"
#include "depth_to_normal_service.h"
#include "gpu.hpp"
#include "hooks.hpp"
#include "lights.hpp"

#include "d/d_kankyo.h"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_OPTIONAL_SERVICE(DepthToNormalService, svc_depth_to_normal);

namespace {

struct Options {
    ConfigVarHandle enabled = 0;
    ConfigVarHandle gi_mode = 0;
    ConfigVarHandle intensity = 0;
    ConfigVarHandle sun_strength = 0;
    ConfigVarHandle radius_scale = 0;
    ConfigVarHandle spec_enabled = 0;
    ConfigVarHandle suppress_native = 0;
    ConfigVarHandle keep_gx_sun = 0;
    ConfigVarHandle half_res_shade = 0;
    ConfigVarHandle ssgi_quality = 0;
    ConfigVarHandle vct_decay = 0;
    ConfigVarHandle debug_view = 0;
} g_options;

GfxDeviceInfo g_device_info = GFX_DEVICE_INFO_INIT;
GfxDrawTypeHandle g_draw_type = 0;
GfxComputeTypeHandle g_compute_type = 0;
GfxStageHookHandle g_after_opaque_hook = 0;
UiWindowHandle g_controls_window = 0;
ResourceBuffer g_common_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_splat_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_normals_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_cluster_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_shade_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_composite_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_ssgi_trace_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_temporal_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_atrous_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_vct_inject_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_vct_resolve_source = RESOURCE_BUFFER_INIT;
ResourceBuffer g_vct_trace_source = RESOURCE_BUFFER_INIT;

uint32_t g_frame_index = 0;
float g_prev_proj_from_world[16]{};
bool g_have_previous_camera = false;
bool g_logged_first_draw = false;
bool g_logged_first_compute = false;
bool g_have_logged_stats = false;
bool g_warned_no_snapshots = false;
uint32_t g_normal_source = 0;  // 0 unknown, 1 internal, 2 Graphics Hub
lighting::GatherStats g_logged_stats{};
uint32_t g_last_stats_log_frame = 0;
float g_last_vct_origins[3][3]{};
bool g_have_vct_origins = false;
uint32_t g_last_vct_frame = 0;

int64_t get_int(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    return handle != 0 && svc_config->get_int(mod_ctx, handle, &value) == MOD_OK ? value : fallback;
}

bool get_bool(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    return handle != 0 && svc_config->get_bool(mod_ctx, handle, &value) == MOD_OK ? value :
                                                                                    fallback;
}

float percent_option(ConfigVarHandle handle, int64_t fallback, int64_t max = 400) {
    return static_cast<float>(std::clamp<int64_t>(get_int(handle, fallback), 0, max)) / 100.0f;
}

float ambient_estimate(const dScnKy_env_light_c& env) {
    const auto channel = [](int value) {
        return std::pow(static_cast<float>(std::clamp(value, 0, 255)) / 255.0f, 2.2f);
    };
    return channel(env.actor_amb_col.r) * 0.2126f + channel(env.actor_amb_col.g) * 0.7152f +
           channel(env.actor_amb_col.b) * 0.0722f;
}

void fill_uniforms(const CameraInfo& camera, const dScnKy_env_light_c& env,
    const lighting::LightList& lights, uint32_t width, uint32_t height,
    lighting::FrameUniforms& uniforms) {
    std::memcpy(uniforms.proj_from_view, camera.proj_from_view, sizeof(uniforms.proj_from_view));
    std::memcpy(uniforms.view_from_proj, camera.view_from_proj, sizeof(uniforms.view_from_proj));
    std::memcpy(uniforms.view_from_world, camera.view_from_world, sizeof(uniforms.view_from_world));
    std::memcpy(uniforms.world_from_view, camera.world_from_view, sizeof(uniforms.world_from_view));
    std::memcpy(uniforms.world_from_proj, camera.world_from_proj, sizeof(uniforms.world_from_proj));
    std::memcpy(uniforms.prev_proj_from_world,
        g_have_previous_camera ? g_prev_proj_from_world : camera.proj_from_world,
        sizeof(uniforms.prev_proj_from_world));
    std::memcpy(uniforms.eye, camera.eye, sizeof(uniforms.eye));
    uniforms.frame_index = g_frame_index;

    // Light splats do not need a scene resolve, so retain an aspect-correct virtual extent there.
    // Deferred passes always provide the exact single-sample target dimensions.
    uniforms.full_size[1] = height != 0 ? static_cast<float>(height) : 1080.0f;
    uniforms.full_size[0] = width != 0 ? static_cast<float>(width) :
                                         std::max(camera.aspect, 0.01f) * uniforms.full_size[1];
    uniforms.inv_full_size[0] = 1.0f / uniforms.full_size[0];
    uniforms.inv_full_size[1] = 1.0f / uniforms.full_size[1];
    uniforms.half_size[0] = std::floor(uniforms.full_size[0] * 0.5f);
    uniforms.half_size[1] = std::floor(uniforms.full_size[1] * 0.5f);
    uniforms.inv_half_size[0] = 1.0f / uniforms.half_size[0];
    uniforms.inv_half_size[1] = 1.0f / uniforms.half_size[1];
    uniforms.near_plane = std::max(camera.near_plane, 0.001f);
    uniforms.far_plane = std::max(camera.far_plane, uniforms.near_plane + 1.0f);
    uniforms.cluster_z_scale = static_cast<float>(lighting::kClusterZ) /
                               std::log(uniforms.far_plane / uniforms.near_plane);
    uniforms.light_count = lights.count;
    uniforms.cluster_dims[0] = lighting::kClusterX;
    uniforms.cluster_dims[1] = lighting::kClusterY;
    uniforms.cluster_dims[2] = lighting::kClusterZ;
    uniforms.gi_mode =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_options.gi_mode, 1), 0, 2));
    uniforms.tile_px[0] = uniforms.full_size[0] / static_cast<float>(lighting::kClusterX);
    uniforms.tile_px[1] = uniforms.full_size[1] / static_cast<float>(lighting::kClusterY);
    uniforms.ambient_estimate = ambient_estimate(env);
    uniforms.master_intensity = percent_option(g_options.intensity, 35);
    uniforms.sun_strength = percent_option(g_options.sun_strength, 15);
    uniforms.knee = 0.35f;
    uniforms.gi_weight = 1.0f;
    uniforms.spec_enabled = get_bool(g_options.spec_enabled, false) ? 1u : 0u;
    const uint32_t quality =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_options.ssgi_quality, 1), 0, 2));
    uniforms.ssgi_rays = 2u + quality;
    uniforms.ssgi_steps = 16;
    uniforms.ssgi_thickness = 50.0f;
    uniforms.temporal_alpha = 0.1f;
    uniforms.units_per_meter = 100.0f;
    uniforms.debug_view =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_options.debug_view, 0), 0, 7));
    uniforms.vct_decay =
        static_cast<float>(std::clamp<int64_t>(get_int(g_options.vct_decay, 995), 900, 1000)) /
        1000.0f;
    uniforms.vct_cone_offset = 1.5f;
    uniforms.vct_voxel0 = 50.0f;
    uniforms.vct_voxel1 = 200.0f;
    uniforms.vct_voxel2 = 800.0f;
    float* origins[] = {uniforms.vct_origin0, uniforms.vct_origin1, uniforms.vct_origin2};
    float* previous[] = {
        uniforms.vct_prev_origin0, uniforms.vct_prev_origin1, uniforms.vct_prev_origin2};
    const float voxel_sizes[] = {uniforms.vct_voxel0, uniforms.vct_voxel1, uniforms.vct_voxel2};
    for (size_t cascade = 0; cascade < 3; ++cascade) {
        for (size_t axis = 0; axis < 3; ++axis) {
            origins[cascade][axis] = std::floor(camera.eye[axis] / voxel_sizes[cascade]) - 32.0f;
            const bool continuous = g_have_vct_origins && g_frame_index == g_last_vct_frame + 1u;
            previous[cascade][axis] =
                continuous ? g_last_vct_origins[cascade][axis] : origins[cascade][axis] + 100000.0f;
        }
    }
}

void commit_vct_origins(const lighting::FrameUniforms& uniforms) {
    std::memcpy(g_last_vct_origins[0], uniforms.vct_origin0, sizeof(g_last_vct_origins[0]));
    std::memcpy(g_last_vct_origins[1], uniforms.vct_origin1, sizeof(g_last_vct_origins[1]));
    std::memcpy(g_last_vct_origins[2], uniforms.vct_origin2, sizeof(g_last_vct_origins[2]));
    g_have_vct_origins = true;
    g_last_vct_frame = g_frame_index;
}

void log_gather_stats(const lighting::LightList& lights, const lighting::GatherStats& stats) {
    const bool changed = !g_have_logged_stats || !(stats == g_logged_stats);
    if (!changed && g_frame_index - g_last_stats_log_frame < 600) {
        return;
    }
    if (changed && g_have_logged_stats && g_frame_index - g_last_stats_log_frame < 30) {
        return;
    }
    char message[320];
    std::snprintf(message, sizeof(message),
        "gathered %u lights: point=%u effect=%u dungeon=%u boss=%u bgparts=%u sun/moon=%u "
        "dedup=%u rejected=%u dropped=%u",
        lights.count, stats.point, stats.effect, stats.dungeon, stats.boss, stats.bgparts,
        stats.directional, stats.duplicates, stats.rejected, stats.dropped);
    svc_log->info(mod_ctx, message);
    g_logged_stats = stats;
    g_have_logged_stats = true;
    g_last_stats_log_frame = g_frame_index;
}

void on_scene_after_opaque(ModContext*, const GfxStageContext* stage, void*) {
    ++g_frame_index;
    lighting::gpu::tick_retired_targets();
    if (!get_bool(g_options.enabled, false) || stage == nullptr ||
        stage->struct_size < sizeof(GfxStageContext) || stage->game_view == nullptr)
    {
        return;
    }

    CameraInfo camera = CAMERA_INFO_INIT;
    if (svc_camera->get_camera(mod_ctx, stage->game_view, &camera) != MOD_OK) {
        return;
    }
    dScnKy_env_light_c* env = dKy_getEnvlight();
    if (env == nullptr) {
        return;
    }

    lighting::GatherParams params;
    params.radius_scale = percent_option(g_options.radius_scale, 100);
    params.sun_strength = percent_option(g_options.sun_strength, 15);
    params.frame_index = g_frame_index;
    lighting::LightList lights{};
    lighting::GatherStats stats{};
    lighting::gather_lights(*env, camera, params, lights, stats);
    log_gather_stats(lights, stats);

    const uint32_t debug_view =
        static_cast<uint32_t>(std::clamp<int64_t>(get_int(g_options.debug_view, 0), 0, 7));
    lighting::FrameUniforms uniforms{};
    GfxRange uniform_range{0, 0};
    GfxRange lights_range{0, 0};
    if (debug_view == 1) {
        fill_uniforms(camera, *env, lights, 0, 0, uniforms);
        if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniform_range) == MOD_OK &&
            svc_gfx->push_storage(mod_ctx, &lights, sizeof(lights), &lights_range) == MOD_OK)
        {
            const lighting::gpu::DrawPayload payload = lighting::gpu::make_splat_payload(
                uniform_range, lights_range, lights.count, debug_view);
            svc_gfx->push_draw(mod_ctx, g_draw_type, &payload, sizeof(payload));
        }
    } else {
        GfxResolveDesc resolve_desc = GFX_RESOLVE_DESC_INIT;
        resolve_desc.color = true;
        resolve_desc.depth = true;
        GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->resolve_pass(mod_ctx, &resolve_desc, &resolved) != MOD_OK ||
            resolved.color == nullptr || resolved.depth == nullptr || resolved.width == 0 ||
            resolved.height == 0)
        {
            if (!g_warned_no_snapshots) {
                g_warned_no_snapshots = true;
                svc_log->warn(
                    mod_ctx, "scene color/depth snapshots unavailable; lighting disabled");
            }
            return;
        }

        WGPUTextureView shared_normals = nullptr;
        if (svc_depth_to_normal != nullptr) {
            DepthToNormalFrame normal_frame = DEPTH_TO_NORMAL_FRAME_INIT;
            if (svc_depth_to_normal->get_frame(mod_ctx, &normal_frame) == MOD_OK &&
                normal_frame.normal != nullptr && normal_frame.width == resolved.width &&
                normal_frame.height == resolved.height)
            {
                shared_normals = normal_frame.normal;
            }
        }
        const uint32_t normal_source = shared_normals != nullptr ? 2u : 1u;
        if (normal_source != g_normal_source) {
            g_normal_source = normal_source;
            svc_log->info(
                mod_ctx, shared_normals != nullptr ?
                             "using Graphics Hub shared depth-to-normal frame" :
                             "Graphics Hub normals unavailable; using internal reconstruction");
        }

        fill_uniforms(camera, *env, lights, resolved.width, resolved.height, uniforms);
        if (svc_gfx->push_uniform(mod_ctx, &uniforms, sizeof(uniforms), &uniform_range) != MOD_OK ||
            svc_gfx->push_storage(mod_ctx, &lights, sizeof(lights), &lights_range) != MOD_OK)
        {
            return;
        }
        lighting::gpu::ComputePayload compute{};
        lighting::gpu::DrawPayload draw{};
        if (!lighting::gpu::prepare_direct_frame(resolved.width, resolved.height,
                get_bool(g_options.half_res_shade, false), resolved, uniform_range, lights_range,
                shared_normals, lights.count, debug_view, uniforms.gi_mode, g_frame_index, compute,
                draw))
        {
            return;
        }
        if (svc_gfx->push_compute(mod_ctx, g_compute_type, &compute, sizeof(compute)) == MOD_OK) {
            if (uniforms.gi_mode == 2u)
                commit_vct_origins(uniforms);
            svc_gfx->push_draw(mod_ctx, g_draw_type, &draw, sizeof(draw));
        }
    }

    std::memcpy(g_prev_proj_from_world, camera.proj_from_world, sizeof(g_prev_proj_from_world));
    g_have_previous_camera = true;
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

void add_select(UiElementHandle pane, const char* label, ConfigVarHandle cvar,
    const char* const* options, size_t count, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.options = options;
    control.option_count = count;
    add_control(pane, control);
}

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "Clustered Lighting");
    add_toggle(left, "Enabled", g_options.enabled, "Runs light gathering and GPU lighting passes.");
    add_number(left, "Intensity", g_options.intensity, 0, 200, 5, "%",
        "Master intensity for the deferred-lighting composite.");
    add_number(left, "Sun Strength", g_options.sun_strength, 0, 100, 5, "%",
        "Directional sun/moon contribution. Twilight Princess already bakes much of this light.");
    add_number(left, "Radius Scale", g_options.radius_scale, 25, 300, 5, "%",
        "Scales gathered point and spotlight influence radii.");
    add_toggle(left, "Specular", g_options.spec_enabled,
        "Adds a conservative Blinn-Phong highlight to depth-derived normals.");
    add_toggle(left, "Half-Resolution Shading", g_options.half_res_shade,
        "Runs direct shading at half resolution and depth-guided upscales it.");

    static const char* kGiModes[] = {"Off", "SSGI", "Voxel Cone Tracing"};
    add_select(left, "GI Mode", g_options.gi_mode, kGiModes, 3,
        "Selects the indirect-lighting implementation. VCT resources are allocated lazily.");
    static const char* kSsgiQuality[] = {"Low", "Medium", "High"};
    add_select(left, "SSGI Quality", g_options.ssgi_quality, kSsgiQuality, 3,
        "Controls the number of cosine-hemisphere rays per half-resolution pixel.");
    add_number(left, "VCT Decay", g_options.vct_decay, 900, 1000, 1, " / 1000",
        "Per-frame retained voxel opacity/radiance. Lower values clear stale screen captures "
        "faster.");

    svc_ui->pane_add_section(mod_ctx, left, "Native Lighting");
    static const char* kSuppressModes[] = {"Off", "Actors", "Full"};
    add_select(left, "Suppress Native", g_options.suppress_native, kSuppressModes, 3,
        "Actors removes the nearest-light TEV boost and actor GX lights. Full also replaces the "
        "global GX loader.");
    add_toggle(left, "Keep GX Sun", g_options.keep_gx_sun,
        "In Full suppression mode, retains sun/moon N dot L for outdoor background geometry.");

    svc_ui->pane_add_section(mod_ctx, left, "Debug");
    static const char* kDebugModes[] = {"Off", "Light Splats", "Normals", "Cluster Heatmap",
        "Direct Only", "GI Only", "Albedo Proxy", "Depth Slices"};
    add_select(left, "Debug View", g_options.debug_view, kDebugModes, 8,
        "Light Splats is the P0 gathering proof. Later views inspect the deferred/GI chain.");
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controls_window = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controls_window != 0) {
        return;
    }
    UiTabDesc tab = UI_TAB_DESC_INIT;
    tab.title = "Controls";
    tab.build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controls_window) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open lighting controls");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_options.enabled;
    add_control(panel, control);
    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool(
    const char* name, bool default_value, ConfigVarHandle& handle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = default_value;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register lighting boolean option");
    }
    return MOD_OK;
}

ModResult register_int(
    const char* name, int64_t default_value, ConfigVarHandle& handle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = default_value;
    if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register lighting integer option");
    }
    return MOD_OK;
}

ModResult register_options(ModError* error) {
    // "enabled" is loader-reserved, so the effect toggle uses an equivalent scoped key.
    ModResult result = register_bool("effect_enabled", false, g_options.enabled, error);
    if (result == MOD_OK)
        result = register_int("gi_mode", 1, g_options.gi_mode, error);
    if (result == MOD_OK)
        result = register_int("intensity", 35, g_options.intensity, error);
    if (result == MOD_OK)
        result = register_int("sun_strength", 15, g_options.sun_strength, error);
    if (result == MOD_OK)
        result = register_int("radius_scale", 100, g_options.radius_scale, error);
    if (result == MOD_OK)
        result = register_bool("spec_enabled", false, g_options.spec_enabled, error);
    if (result == MOD_OK)
        result = register_int("suppress_native", 1, g_options.suppress_native, error);
    if (result == MOD_OK)
        result = register_bool("keep_gx_sun", true, g_options.keep_gx_sun, error);
    if (result == MOD_OK)
        result = register_bool("half_res_shade", false, g_options.half_res_shade, error);
    if (result == MOD_OK)
        result = register_int("ssgi_quality", 1, g_options.ssgi_quality, error);
    if (result == MOD_OK)
        result = register_int("vct_decay", 995, g_options.vct_decay, error);
    if (result == MOD_OK)
        result = register_int("debug_view", 0, g_options.debug_view, error);
    return result;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult result = svc_resource->load(mod_ctx, "common.wgsl", &g_common_source);
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "splat.wgsl", &g_splat_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "normals.wgsl", &g_normals_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "cluster_cull.wgsl", &g_cluster_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "shade.wgsl", &g_shade_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "composite.wgsl", &g_composite_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "ssgi_trace.wgsl", &g_ssgi_trace_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "filter_temporal.wgsl", &g_temporal_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "filter_atrous.wgsl", &g_atrous_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "vct_inject.wgsl", &g_vct_inject_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "vct_resolve.wgsl", &g_vct_resolve_source);
    }
    if (result == MOD_OK) {
        result = svc_resource->load(mod_ctx, "vct_trace.wgsl", &g_vct_trace_source);
    }
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to load lighting shaders");
    }
    result = register_options(error);
    if (result != MOD_OK) {
        return result;
    }
    if (svc_gfx->get_device_info(mod_ctx, &g_device_info) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to query WebGPU device info");
    }
    if (!lighting::gpu::initialize(g_device_info, g_common_source, g_splat_source, g_normals_source,
            g_cluster_source, g_shade_source, g_composite_source, g_ssgi_trace_source,
            g_temporal_source, g_atrous_source, g_vct_inject_source, g_vct_resolve_source,
            g_vct_trace_source))
    {
        return mods::set_error(error, MOD_ERROR, "failed to create lighting GPU pipelines");
    }

    GfxComputeTypeDesc compute_desc = GFX_COMPUTE_TYPE_DESC_INIT;
    compute_desc.label = "lighting normals/cull/shade";
    compute_desc.callback = lighting::gpu::on_compute;
    if (svc_gfx->register_compute_type(mod_ctx, &compute_desc, &g_compute_type) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register lighting compute type");
    }

    GfxDrawTypeDesc draw_desc = GFX_DRAW_TYPE_DESC_INIT;
    draw_desc.label = "lighting composite/debug";
    draw_desc.draw = lighting::gpu::on_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &draw_desc, &g_draw_type) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register lighting draw type");
    }
    GfxStageHookDesc stage_desc = GFX_STAGE_HOOK_DESC_INIT;
    stage_desc.callback = on_scene_after_opaque;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_SCENE_AFTER_OPAQUE, &stage_desc, &g_after_opaque_hook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register after-opaque lighting hook");
    }
    const lighting::hooks::Options hook_options{
        svc_config, g_options.enabled, g_options.suppress_native, g_options.keep_gx_sun};
    result = lighting::hooks::initialize(svc_hook, hook_options, error);
    if (result != MOD_OK) {
        return result;
    }

    UiModsPanelDesc panel_desc = UI_MODS_PANEL_DESC_INIT;
    panel_desc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel_desc);
    svc_log->info(
        mod_ctx, "lighting_mod ready: clustered direct lighting, SSGI, VCT, and suppression hooks");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (!g_logged_first_compute && lighting::gpu::consume_first_compute()) {
        g_logged_first_compute = true;
        svc_log->info(mod_ctx, "lighting normals/cull/shade compute chain executed OK");
    }
    if (!g_logged_first_draw && lighting::gpu::consume_first_draw()) {
        g_logged_first_draw = true;
        svc_log->info(mod_ctx, "lighting composite/debug draw executed OK");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    lighting::hooks::shutdown();
    lighting::gpu::shutdown();
    svc_resource->free(mod_ctx, &g_common_source);
    svc_resource->free(mod_ctx, &g_splat_source);
    svc_resource->free(mod_ctx, &g_normals_source);
    svc_resource->free(mod_ctx, &g_cluster_source);
    svc_resource->free(mod_ctx, &g_shade_source);
    svc_resource->free(mod_ctx, &g_composite_source);
    svc_resource->free(mod_ctx, &g_ssgi_trace_source);
    svc_resource->free(mod_ctx, &g_temporal_source);
    svc_resource->free(mod_ctx, &g_atrous_source);
    svc_resource->free(mod_ctx, &g_vct_inject_source);
    svc_resource->free(mod_ctx, &g_vct_resolve_source);
    svc_resource->free(mod_ctx, &g_vct_trace_source);
    g_options = {};
    g_device_info = GFX_DEVICE_INFO_INIT;
    g_draw_type = 0;
    g_compute_type = 0;
    g_after_opaque_hook = 0;
    g_controls_window = 0;
    g_frame_index = 0;
    g_have_previous_camera = false;
    g_logged_first_draw = false;
    g_logged_first_compute = false;
    g_have_logged_stats = false;
    g_warned_no_snapshots = false;
    g_normal_source = 0;
    g_have_vct_origins = false;
    g_last_vct_frame = 0;
    return MOD_OK;
}

}  // extern "C"
