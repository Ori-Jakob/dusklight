#pragma once

#include "common.hpp"

#include "mods/svc/gfx.h"
#include "mods/svc/resource.h"

#include <cstddef>
#include <cstdint>

struct ModContext;

namespace lighting::gpu {

enum DrawKind : uint32_t {
    DrawKindSplat = 0,
    DrawKindComposite = 1,
};

struct DrawPayload {
    WGPUTextureView scene_color;
    WGPUTextureView scene_depth;
    WGPUTextureView normals;
    WGPUTextureView light_hdr;
    WGPUTextureView gi_filtered;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t lights_offset;
    uint32_t lights_size;
    uint32_t light_count;
    uint32_t debug_view;
    uint32_t kind;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

struct ComputePayload {
    WGPUTextureView scene_color;
    WGPUTextureView scene_depth;
    WGPUTextureView normals;
    WGPUTextureView light_hdr;
    WGPUTextureView gi_raw;
    WGPUTextureView gi_history_read;
    WGPUTextureView gi_history_write;
    WGPUTextureView gi_filter_a;
    WGPUTextureView gi_filter_b;
    WGPUBuffer clusters;
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t lights_offset;
    uint32_t lights_size;
    uint32_t full_width;
    uint32_t full_height;
    uint32_t shade_width;
    uint32_t shade_height;
    uint32_t reconstruct_normals;
    uint32_t gi_mode;
    uint32_t frame_index;
};
static_assert(sizeof(ComputePayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& splat_source, const ResourceBuffer& normals_source,
    const ResourceBuffer& cluster_source, const ResourceBuffer& shade_source,
    const ResourceBuffer& composite_source, const ResourceBuffer& ssgi_trace_source,
    const ResourceBuffer& temporal_source, const ResourceBuffer& atrous_source,
    const ResourceBuffer& vct_inject_source, const ResourceBuffer& vct_resolve_source,
    const ResourceBuffer& vct_trace_source);
void shutdown();
void tick_retired_targets();
bool prepare_direct_frame(uint32_t width, uint32_t height, bool half_res,
    const GfxResolvedTargets& resolved, GfxRange uniform_range, GfxRange lights_range,
    WGPUTextureView shared_normals, uint32_t light_count, uint32_t debug_view, uint32_t gi_mode,
    uint32_t frame_index, ComputePayload& compute, DrawPayload& draw);
DrawPayload make_splat_payload(
    GfxRange uniform_range, GfxRange lights_range, uint32_t light_count, uint32_t debug_view);
void on_compute(ModContext*, const GfxComputeContext*, const void*, size_t, void*);
void on_draw(ModContext*, const GfxDrawContext*, const void*, size_t, void*);
bool consume_first_draw();
bool consume_first_compute();

}  // namespace lighting::gpu
