#pragma once

#include "mods/svc/gfx.h"
#include "mods/svc/resource.h"
#include "ssgi.hpp"

namespace lighting::vct {

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& inject_source, const ResourceBuffer& resolve_source,
    const ResourceBuffer& trace_source);
void shutdown();
bool prepare_frame();
bool record(WGPUComputePassEncoder pass, const GfxComputeContext& context, GfxRange uniform_range,
    WGPUTextureView scene_color, WGPUTextureView scene_depth, WGPUTextureView normals,
    WGPUTextureView light_hdr, WGPUTextureView gi_raw, uint32_t width, uint32_t height,
    uint32_t frame_index);

}  // namespace lighting::vct
