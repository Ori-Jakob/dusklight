#pragma once

#include "common.hpp"

#include <cstdint>

struct CameraInfo;
class dScnKy_env_light_c;

namespace lighting {

struct GatherParams {
    float radius_scale = 1.0f;
    float sun_strength = 0.15f;
    uint32_t frame_index = 0;
};

struct GatherStats {
    uint32_t point = 0;
    uint32_t effect = 0;
    uint32_t dungeon = 0;
    uint32_t boss = 0;
    uint32_t bgparts = 0;
    uint32_t directional = 0;
    uint32_t duplicates = 0;
    uint32_t rejected = 0;
    uint32_t dropped = 0;

    bool operator==(const GatherStats&) const = default;
};

void gather_lights(const dScnKy_env_light_c& env, const CameraInfo& camera,
    const GatherParams& params, LightList& out_lights, GatherStats& out_stats);

}  // namespace lighting
