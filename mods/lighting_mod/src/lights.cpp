#include "lights.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "mods/svc/camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lighting {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSunMoonDistance = 80000.0f;
constexpr float kSunMoonZDistance = 10000.0f;

float clamp_color(int value) {
    return static_cast<float>(std::clamp(value, 0, 255)) / 255.0f;
}

void linear_color(int r, int g, int b, float out[3]) {
    out[0] = std::pow(clamp_color(r), 2.2f);
    out[1] = std::pow(clamp_color(g), 2.2f);
    out[2] = std::pow(clamp_color(b), 2.2f);
}

bool color_is_black(int r, int g, int b) {
    return r <= 0 && g <= 0 && b <= 0;
}

float wrap_daytime(float daytime) {
    if (!std::isfinite(daytime)) {
        return 180.0f;
    }
    float wrapped = std::fmod(daytime, 360.0f);
    return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
}

float daytime_percent(float max, float min, float value) {
    const float range = max - min;
    if (range == 0.0f) {
        return 1.0f;
    }
    return std::min(1.0f, 1.0f - ((max - value) / range));
}

float sun_moon_angle(float daytime) {
    daytime = wrap_daytime(daytime);
    if (daytime >= 90.0f && daytime <= 270.0f) {
        return daytime_percent(270.0f, 90.0f, daytime) * 150.0f + 105.0f;
    }
    float angle = daytime < 90.0f ? daytime + 360.0f : daytime;
    angle = daytime_percent(450.0f, 270.0f, angle) * 210.0f + 255.0f;
    return angle > 360.0f ? angle - 360.0f : angle;
}

void sun_moon_offset(float daytime, float out[3]) {
    const float angle = sun_moon_angle(daytime) * kPi / 180.0f;
    out[0] = std::sin(angle) * kSunMoonDistance;
    out[1] = -std::cos(angle) * kSunMoonDistance;
    out[2] = std::cos(angle) * kSunMoonZDistance;
}

float length3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void normalize3(float v[3]) {
    const float length = length3(v);
    if (length > 1.0e-6f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

void transform_direction(const float matrix[16], const float in[3], float out[3]) {
    // Camera matrices are column-major. Ignore translation for a direction.
    out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
    out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
    out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
}

void spot_direction(float angle_x, float angle_y, const CameraInfo& camera, float out[3]) {
    const float x = angle_x * kPi / 180.0f;
    const float y = angle_y * kPi / 180.0f;
    const float local[3] = {std::cos(x) * std::cos(y), std::sin(x), std::cos(x) * std::sin(y)};
    transform_direction(camera.world_from_view, local, out);
    out[0] = -out[0];
    out[1] = -out[1];
    out[2] = -out[2];
    normalize3(out);
}

float flicker_multiplier(float fluctuation, uint32_t frame, uint32_t salt) {
    const float amount = std::clamp(fluctuation / 255.0f, 0.0f, 1.0f);
    if (amount <= 0.0f) {
        return 1.0f;
    }
    const float phase = static_cast<float>(frame) * 0.173f + static_cast<float>(salt) * 1.618f;
    const float wave = std::sin(phase) * 0.65f + std::sin(phase * 0.37f + 2.1f) * 0.35f;
    return std::max(0.2f, 1.0f + wave * amount * 0.35f);
}

bool same_position(const GpuLight& light, const float pos[3]) {
    constexpr float kEpsilon = 0.5f;
    const float dx = light.pos_ws[0] - pos[0];
    const float dy = light.pos_ws[1] - pos[1];
    const float dz = light.pos_ws[2] - pos[2];
    return dx * dx + dy * dy + dz * dz <= kEpsilon * kEpsilon;
}

bool is_duplicate(const LightList& lights, const float pos[3]) {
    for (uint32_t i = lights.directional_count; i < lights.count; ++i) {
        if (same_position(lights.lights[i], pos)) {
            return true;
        }
    }
    return false;
}

bool append(LightList& lights, const GpuLight& light, GatherStats& stats) {
    if (lights.count >= kMaxLights) {
        ++stats.dropped;
        return false;
    }
    lights.lights[lights.count++] = light;
    return true;
}

bool append_influence(const LIGHT_INFLUENCE& source, float radius_scale, uint32_t frame,
    uint32_t salt, LightList& lights, GatherStats& stats, bool deduplicate) {
    if (!std::isfinite(source.mPow) || source.mPow <= 0.0f ||
        color_is_black(source.mColor.r, source.mColor.g, source.mColor.b))
    {
        ++stats.rejected;
        return false;
    }

    const float pos[3] = {source.mPosition.x, source.mPosition.y, source.mPosition.z};
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
        ++stats.rejected;
        return false;
    }
    if (deduplicate && is_duplicate(lights, pos)) {
        ++stats.duplicates;
        return false;
    }

    GpuLight light{};
    std::memcpy(light.pos_ws, pos, sizeof(pos));
    light.radius = std::clamp((250.0f + source.mPow) * radius_scale, 100.0f, 5000.0f);
    linear_color(source.mColor.r, source.mColor.g, source.mColor.b, light.color);
    light.intensity = flicker_multiplier(source.mFluctuation, frame, salt);
    light.light_type = LightTypePoint;
    light.cos_inner = 1.0f;
    light.cos_outer = -1.0f;
    return append(lights, light, stats);
}

template <typename Spot>
bool append_spot(const Spot& source, const cXyz& position, const GXColor& color,
    const CameraInfo& camera, float radius_scale, LightList& lights, GatherStats& stats) {
    if (!std::isfinite(source.mRefDistance) || source.mRefDistance <= 0.0f ||
        color_is_black(color.r, color.g, color.b))
    {
        ++stats.rejected;
        return false;
    }
    GpuLight light{};
    light.pos_ws[0] = position.x;
    light.pos_ws[1] = position.y;
    light.pos_ws[2] = position.z;
    light.radius = std::clamp(source.mRefDistance * radius_scale, 100.0f, 5000.0f);
    linear_color(color.r, color.g, color.b, light.color);
    light.intensity = 1.0f;
    spot_direction(source.mAngleX, source.mAngleY, camera, light.dir_ws);
    light.light_type = LightTypeSpot;
    const float cutoff = std::clamp(source.mCutoffAngle, 1.0f, 89.9f) * kPi / 180.0f;
    light.cos_inner = std::cos(cutoff * 0.8f);
    light.cos_outer = std::cos(cutoff);
    return append(lights, light, stats);
}

}  // namespace

void gather_lights(const dScnKy_env_light_c& env, const CameraInfo& camera,
    const GatherParams& params, LightList& out_lights, GatherStats& out_stats) {
    out_lights = {};
    out_stats = {};

    // Keep the directional prefix invariant used by the cluster pass.
    float sun[3];
    float daytime = wrap_daytime(dComIfGs_getTime());
    sun_moon_offset(daytime, sun);
    if (sun[1] <= 0.0f) {
        sun_moon_offset(daytime + 180.0f, sun);
    }
    normalize3(sun);
    const float horizon_fade = std::clamp((sun[1] - 0.05f) / 0.15f, 0.0f, 1.0f);
    if (params.sun_strength > 0.0f && horizon_fade > 0.0f &&
        !color_is_black(env.base_light.mColor.r, env.base_light.mColor.g, env.base_light.mColor.b))
    {
        GpuLight light{};
        std::memcpy(light.dir_ws, sun, sizeof(sun));
        linear_color(
            env.base_light.mColor.r, env.base_light.mColor.g, env.base_light.mColor.b, light.color);
        light.intensity = params.sun_strength * horizon_fade;
        light.light_type = LightTypeDirectional;
        if (append(out_lights, light, out_stats)) {
            out_lights.directional_count = 1;
            out_stats.directional = 1;
        }
    }

    for (uint32_t i = 0; i < 100; ++i) {
        if (env.pointlight[i] != nullptr &&
            append_influence(*env.pointlight[i], params.radius_scale, params.frame_index, i,
                out_lights, out_stats, false))
        {
            ++out_stats.point;
        }
    }
    for (uint32_t i = 0; i < 5; ++i) {
        if (env.efplight[i] != nullptr &&
            append_influence(*env.efplight[i], params.radius_scale, params.frame_index, 100 + i,
                out_lights, out_stats, false))
        {
            ++out_stats.effect;
        }
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const DUNGEON_LIGHT& spot = env.dungeonlight[i];
        if (append_spot(spot, spot.mPosition, spot.mColor, camera, params.radius_scale, out_lights,
                out_stats))
        {
            ++out_stats.dungeon;
        }
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const BOSS_LIGHT& spot = env.field_0x0c18[i];
        if (spot.field_0x26 == 1 && append_spot(spot, spot.mPos, spot.mColor, camera,
                                        params.radius_scale, out_lights, out_stats))
        {
            ++out_stats.boss;
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const LIGHT_INFLUENCE& light = env.bgparts_active_light[i];
        if (light.mIndex != 0 && append_influence(light, params.radius_scale, params.frame_index,
                                     120 + i, out_lights, out_stats, true))
        {
            ++out_stats.bgparts;
        }
    }
}

}  // namespace lighting
