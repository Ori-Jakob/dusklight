#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lighting {

constexpr uint32_t kMaxLights = 256;
constexpr uint32_t kClusterX = 16;
constexpr uint32_t kClusterY = 9;
constexpr uint32_t kClusterZ = 24;
constexpr uint32_t kClusterStride = 64;

enum LightType : uint32_t {
    LightTypePoint = 0,
    LightTypeSpot = 1,
    LightTypeDirectional = 2,
};

// Mirrors GpuLight in res/common.wgsl.
struct alignas(16) GpuLight {
    float pos_ws[3];
    float radius;
    float color[3];
    float intensity;
    float dir_ws[3];
    uint32_t light_type;
    float cos_inner;
    float cos_outer;
    float pad[2];
};
static_assert(sizeof(GpuLight) == 64);
static_assert(std::is_trivially_copyable_v<GpuLight>);

// Directional lights are stored first and bypass clustered culling.
struct alignas(16) LightList {
    uint32_t count;
    uint32_t directional_count;
    uint32_t pad[2];
    GpuLight lights[kMaxLights];
};
static_assert(sizeof(LightList) == 16 + sizeof(GpuLight) * kMaxLights);
static_assert(std::is_trivially_copyable_v<LightList>);

// Mirrors FrameUniforms in res/common.wgsl. Fields are grouped into 16-byte lanes.
struct alignas(16) FrameUniforms {
    float proj_from_view[16];
    float view_from_proj[16];
    float view_from_world[16];
    float world_from_view[16];
    float world_from_proj[16];
    float prev_proj_from_world[16];

    float eye[3];
    uint32_t frame_index;
    float full_size[2];
    float inv_full_size[2];
    float half_size[2];
    float inv_half_size[2];
    float near_plane;
    float far_plane;
    float cluster_z_scale;
    uint32_t light_count;
    uint32_t cluster_dims[3];
    uint32_t gi_mode;
    float tile_px[2];
    float ambient_estimate;
    float master_intensity;
    float sun_strength;
    float knee;
    float gi_weight;
    uint32_t spec_enabled;
    uint32_t ssgi_rays;
    uint32_t ssgi_steps;
    float ssgi_thickness;
    float temporal_alpha;
    float units_per_meter;
    uint32_t debug_view;
    float vct_decay;
    float vct_cone_offset;
    float vct_origin0[3];
    float vct_voxel0;
    float vct_origin1[3];
    float vct_voxel1;
    float vct_origin2[3];
    float vct_voxel2;
    float vct_prev_origin0[3];
    float vct_pad0;
    float vct_prev_origin1[3];
    float vct_pad1;
    float vct_prev_origin2[3];
    float vct_pad2;
};
static_assert(sizeof(FrameUniforms) == 624);
static_assert(sizeof(FrameUniforms) % 16 == 0);
static_assert(std::is_trivially_copyable_v<FrameUniforms>);

}  // namespace lighting
