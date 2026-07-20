#pragma once

#include "mods/svc/gfx.h"
#include "mods/svc/resource.h"

#include <webgpu/webgpu.h>

namespace lighting::ssgi {

struct FrameViews {
    WGPUTextureView raw = nullptr;
    WGPUTextureView history_read = nullptr;
    WGPUTextureView history_write = nullptr;
    WGPUTextureView filter_a = nullptr;
    WGPUTextureView filter_b = nullptr;
};

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& trace_source, const ResourceBuffer& temporal_source,
    const ResourceBuffer& atrous_source);
void shutdown();
void tick_retired_targets();
bool prepare_frame(
    uint32_t width, uint32_t height, uint32_t frame_index, uint32_t gi_mode, FrameViews& views);
bool record(WGPUComputePassEncoder pass, const GfxComputeContext& context, GfxRange uniform_range,
    WGPUTextureView scene_color, WGPUTextureView scene_depth, WGPUTextureView normals,
    WGPUTextureView light_hdr, const FrameViews& views, uint32_t width, uint32_t height,
    bool run_trace);

}  // namespace lighting::ssgi
