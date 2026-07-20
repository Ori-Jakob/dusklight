#include "vct.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string>

namespace lighting::vct {
namespace {

constexpr uint32_t kDimension = 64;
constexpr uint32_t kCascades = 3;
constexpr uint32_t kMipCount = 7;
constexpr uint64_t kVoxelCount =
    static_cast<uint64_t>(kDimension) * kDimension * kDimension * kCascades;
constexpr uint64_t kAccumSize = kVoxelCount * 4u * sizeof(uint32_t);

GfxDeviceInfo g_device_info = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_inject_pipeline = nullptr;
WGPUComputePipeline g_resolve_pipeline = nullptr;
WGPUComputePipeline g_downsample_pipeline = nullptr;
WGPUComputePipeline g_trace_pipeline = nullptr;
WGPUBindGroupLayout g_inject_layout = nullptr;
WGPUBindGroupLayout g_resolve_layout = nullptr;
WGPUBindGroupLayout g_downsample_layout = nullptr;
WGPUBindGroupLayout g_trace_layout = nullptr;

struct Resources {
    WGPUTexture clipmaps[2][kCascades]{};
    WGPUTextureView all_mips[2][kCascades]{};
    WGPUTextureView mip_views[2][kCascades][kMipCount]{};
    WGPUBindGroup downsample_groups[2][kCascades][kMipCount - 1]{};
    WGPUBuffer accum = nullptr;
    bool ready = false;
};
Resources g_resources;

constexpr uint32_t div_ceil(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1u) / divisor;
}

WGPUShaderModule create_shader(
    const char* label, const ResourceBuffer& common, const ResourceBuffer& source) {
    std::string joined;
    joined.reserve(common.size + source.size + 2);
    joined.append(static_cast<const char*>(common.data), common.size);
    joined.push_back('\n');
    joined.append(static_cast<const char*>(source.data), source.size);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {joined.data(), joined.size()};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &wgsl.chain;
    desc.label = {label, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(g_device_info.device, &desc);
}

bool build_pipeline(const char* label, const char* entry, const ResourceBuffer& common,
    const ResourceBuffer& source, WGPUComputePipeline& pipeline, WGPUBindGroupLayout& layout) {
    WGPUShaderModule module = create_shader(label, common, source);
    if (module == nullptr)
        return false;
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {label, WGPU_STRLEN};
    desc.compute.module = module;
    desc.compute.entryPoint = {entry, WGPU_STRLEN};
    pipeline = wgpuDeviceCreateComputePipeline(g_device_info.device, &desc);
    wgpuShaderModuleRelease(module);
    if (pipeline == nullptr)
        return false;
    layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    return layout != nullptr;
}

WGPUBindGroupEntry texture_entry(uint32_t binding, WGPUTextureView view) {
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = binding;
    entry.textureView = view;
    return entry;
}

WGPUBindGroupEntry buffer_entry(
    uint32_t binding, WGPUBuffer buffer, uint64_t offset, uint64_t size) {
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = binding;
    entry.buffer = buffer;
    entry.offset = offset;
    entry.size = size;
    return entry;
}

WGPUBindGroup make_group(
    WGPUBindGroupLayout layout, std::initializer_list<WGPUBindGroupEntry> entries) {
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = layout;
    desc.entryCount = entries.size();
    desc.entries = entries.begin();
    return wgpuDeviceCreateBindGroup(g_device_info.device, &desc);
}

void release_resources() {
    for (auto& set : g_resources.downsample_groups) {
        for (auto& cascade : set) {
            for (WGPUBindGroup& group : cascade) {
                if (group != nullptr)
                    wgpuBindGroupRelease(group);
                group = nullptr;
            }
        }
    }
    for (auto& set : g_resources.mip_views) {
        for (auto& cascade : set) {
            for (WGPUTextureView& view : cascade) {
                if (view != nullptr)
                    wgpuTextureViewRelease(view);
                view = nullptr;
            }
        }
    }
    for (auto& set : g_resources.all_mips) {
        for (WGPUTextureView& view : set) {
            if (view != nullptr)
                wgpuTextureViewRelease(view);
            view = nullptr;
        }
    }
    for (auto& set : g_resources.clipmaps) {
        for (WGPUTexture& texture : set) {
            if (texture != nullptr)
                wgpuTextureRelease(texture);
            texture = nullptr;
        }
    }
    if (g_resources.accum != nullptr)
        wgpuBufferRelease(g_resources.accum);
    g_resources = {};
}

bool allocate_resources() {
    if (g_resources.ready)
        return true;
    for (uint32_t set = 0; set < 2; ++set) {
        for (uint32_t cascade = 0; cascade < kCascades; ++cascade) {
            WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            desc.label = {
                set == 0 ? "lighting VCT clipmap A" : "lighting VCT clipmap B", WGPU_STRLEN};
            desc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
            desc.dimension = WGPUTextureDimension_3D;
            desc.size = {kDimension, kDimension, kDimension};
            desc.format = WGPUTextureFormat_RGBA16Float;
            desc.mipLevelCount = kMipCount;
            g_resources.clipmaps[set][cascade] =
                wgpuDeviceCreateTexture(g_device_info.device, &desc);
            if (g_resources.clipmaps[set][cascade] == nullptr) {
                release_resources();
                return false;
            }
            g_resources.all_mips[set][cascade] =
                wgpuTextureCreateView(g_resources.clipmaps[set][cascade], nullptr);
            if (g_resources.all_mips[set][cascade] == nullptr) {
                release_resources();
                return false;
            }
            for (uint32_t mip = 0; mip < kMipCount; ++mip) {
                WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                view_desc.dimension = WGPUTextureViewDimension_3D;
                view_desc.baseMipLevel = mip;
                view_desc.mipLevelCount = 1;
                g_resources.mip_views[set][cascade][mip] =
                    wgpuTextureCreateView(g_resources.clipmaps[set][cascade], &view_desc);
                if (g_resources.mip_views[set][cascade][mip] == nullptr) {
                    release_resources();
                    return false;
                }
            }
        }
    }
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = {"lighting VCT atomic accumulation", WGPU_STRLEN};
    buffer_desc.usage = WGPUBufferUsage_Storage;
    buffer_desc.size = kAccumSize;
    g_resources.accum = wgpuDeviceCreateBuffer(g_device_info.device, &buffer_desc);
    if (g_resources.accum == nullptr) {
        release_resources();
        return false;
    }
    for (uint32_t set = 0; set < 2; ++set) {
        for (uint32_t cascade = 0; cascade < kCascades; ++cascade) {
            for (uint32_t mip = 1; mip < kMipCount; ++mip) {
                g_resources.downsample_groups[set][cascade][mip - 1] =
                    make_group(g_downsample_layout,
                        {texture_entry(8, g_resources.mip_views[set][cascade][mip - 1]),
                            texture_entry(9, g_resources.mip_views[set][cascade][mip])});
                if (g_resources.downsample_groups[set][cascade][mip - 1] == nullptr) {
                    release_resources();
                    return false;
                }
            }
        }
    }
    g_resources.ready = true;
    return true;
}

}  // namespace

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& inject_source, const ResourceBuffer& resolve_source,
    const ResourceBuffer& trace_source) {
    g_device_info = device_info;
    return build_pipeline("lighting VCT inject", "inject_voxels", common_source, inject_source,
               g_inject_pipeline, g_inject_layout) &&
           build_pipeline("lighting VCT resolve", "resolve_voxels", common_source, resolve_source,
               g_resolve_pipeline, g_resolve_layout) &&
           build_pipeline("lighting VCT mip downsample", "downsample_voxels", common_source,
               resolve_source, g_downsample_pipeline, g_downsample_layout) &&
           build_pipeline("lighting VCT trace", "trace_vct", common_source, trace_source,
               g_trace_pipeline, g_trace_layout);
}

bool prepare_frame() {
    return allocate_resources();
}

bool record(WGPUComputePassEncoder pass, const GfxComputeContext& context, GfxRange uniform_range,
    WGPUTextureView scene_color, WGPUTextureView scene_depth, WGPUTextureView normals,
    WGPUTextureView light_hdr, WGPUTextureView gi_raw, uint32_t width, uint32_t height,
    uint32_t frame_index) {
    if (!g_resources.ready || pass == nullptr || scene_color == nullptr || scene_depth == nullptr ||
        normals == nullptr || light_hdr == nullptr || gi_raw == nullptr)
    {
        return false;
    }
    const uint32_t write = frame_index & 1u;
    const uint32_t read = write ^ 1u;
    const WGPUBindGroupEntry uniform =
        buffer_entry(0, context.uniform_buffer, uniform_range.offset, uniform_range.size);
    WGPUBindGroup inject = make_group(
        g_inject_layout, {uniform, texture_entry(1, scene_color), texture_entry(2, scene_depth),
                             texture_entry(3, normals), texture_entry(4, light_hdr),
                             buffer_entry(5, g_resources.accum, 0, kAccumSize)});
    WGPUBindGroup resolve =
        make_group(g_resolve_layout, {uniform, buffer_entry(1, g_resources.accum, 0, kAccumSize),
                                         texture_entry(2, g_resources.all_mips[read][0]),
                                         texture_entry(3, g_resources.all_mips[read][1]),
                                         texture_entry(4, g_resources.all_mips[read][2]),
                                         texture_entry(5, g_resources.mip_views[write][0][0]),
                                         texture_entry(6, g_resources.mip_views[write][1][0]),
                                         texture_entry(7, g_resources.mip_views[write][2][0])});
    WGPUBindGroup trace = make_group(g_trace_layout,
        {uniform, texture_entry(1, scene_depth), texture_entry(2, normals),
            texture_entry(3, g_resources.all_mips[write][0]),
            texture_entry(4, g_resources.all_mips[write][1]),
            texture_entry(5, g_resources.all_mips[write][2]), texture_entry(6, gi_raw)});
    if (inject == nullptr || resolve == nullptr || trace == nullptr) {
        if (inject != nullptr)
            wgpuBindGroupRelease(inject);
        if (resolve != nullptr)
            wgpuBindGroupRelease(resolve);
        if (trace != nullptr)
            wgpuBindGroupRelease(trace);
        return false;
    }

    const uint32_t half_width = std::max(1u, width / 2u);
    const uint32_t half_height = std::max(1u, height / 2u);
    wgpuComputePassEncoderSetPipeline(pass, g_inject_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, inject, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(half_width, 8), div_ceil(half_height, 8), 1);
    wgpuComputePassEncoderSetPipeline(pass, g_resolve_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, resolve, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, div_ceil(kDimension, 4), div_ceil(kDimension, 4),
        div_ceil(kDimension * kCascades, 4));
    wgpuComputePassEncoderSetPipeline(pass, g_downsample_pipeline);
    for (uint32_t cascade = 0; cascade < kCascades; ++cascade) {
        for (uint32_t mip = 1; mip < kMipCount; ++mip) {
            const uint32_t dimension = kDimension >> mip;
            wgpuComputePassEncoderSetBindGroup(
                pass, 0, g_resources.downsample_groups[write][cascade][mip - 1], 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, div_ceil(dimension, 4), div_ceil(dimension, 4), div_ceil(dimension, 4));
        }
    }
    wgpuComputePassEncoderSetPipeline(pass, g_trace_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, trace, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(half_width, 8), div_ceil(half_height, 8), 1);
    wgpuBindGroupRelease(inject);
    wgpuBindGroupRelease(resolve);
    wgpuBindGroupRelease(trace);
    return true;
}

void shutdown() {
    release_resources();
    WGPUComputePipeline* pipelines[] = {
        &g_inject_pipeline, &g_resolve_pipeline, &g_downsample_pipeline, &g_trace_pipeline};
    for (WGPUComputePipeline* pipeline : pipelines) {
        if (*pipeline != nullptr)
            wgpuComputePipelineRelease(*pipeline);
        *pipeline = nullptr;
    }
    WGPUBindGroupLayout* layouts[] = {
        &g_inject_layout, &g_resolve_layout, &g_downsample_layout, &g_trace_layout};
    for (WGPUBindGroupLayout* layout : layouts) {
        if (*layout != nullptr)
            wgpuBindGroupLayoutRelease(*layout);
        *layout = nullptr;
    }
    g_device_info = GFX_DEVICE_INFO_INIT;
}

}  // namespace lighting::vct
