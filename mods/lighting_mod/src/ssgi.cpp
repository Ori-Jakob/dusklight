#include "ssgi.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace lighting::ssgi {
namespace {

GfxDeviceInfo g_device_info = GFX_DEVICE_INFO_INIT;
WGPUComputePipeline g_trace_pipeline = nullptr;
WGPUComputePipeline g_temporal_pipeline = nullptr;
WGPUComputePipeline g_atrous_pipelines[3]{};
WGPUBindGroupLayout g_trace_layout = nullptr;
WGPUBindGroupLayout g_temporal_layout = nullptr;
WGPUBindGroupLayout g_atrous_layouts[3]{};

struct Targets {
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTexture textures[5]{};
    WGPUTextureView views[5]{};
};
Targets g_targets;
struct RetiredTargets {
    Targets targets;
    int frames_left;
};
std::vector<RetiredTargets> g_retired;
uint32_t g_last_frame = 0;
uint32_t g_last_mode = 0;
bool g_have_last_frame = false;

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

void release_targets(Targets& targets) {
    for (WGPUTextureView& view : targets.views) {
        if (view != nullptr)
            wgpuTextureViewRelease(view);
        view = nullptr;
    }
    for (WGPUTexture& texture : targets.textures) {
        if (texture != nullptr)
            wgpuTextureRelease(texture);
        texture = nullptr;
    }
    targets = {};
}

bool ensure_targets(uint32_t width, uint32_t height) {
    const uint32_t half_width = std::max(1u, width / 2u);
    const uint32_t half_height = std::max(1u, height / 2u);
    if (g_targets.width == half_width && g_targets.height == half_height)
        return true;
    if (g_targets.width != 0) {
        g_retired.push_back({std::exchange(g_targets, Targets{}), 4});
    }
    static constexpr const char* kLabels[] = {"lighting SSGI raw", "lighting SSGI history A",
        "lighting SSGI history B", "lighting SSGI filter A", "lighting SSGI filter B"};
    for (size_t i = 0; i < std::size(g_targets.textures); ++i) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = {kLabels[i], WGPU_STRLEN};
        desc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        desc.size = {half_width, half_height, 1};
        desc.format = WGPUTextureFormat_RGBA16Float;
        g_targets.textures[i] = wgpuDeviceCreateTexture(g_device_info.device, &desc);
        if (g_targets.textures[i] == nullptr) {
            release_targets(g_targets);
            return false;
        }
        g_targets.views[i] = wgpuTextureCreateView(g_targets.textures[i], nullptr);
        if (g_targets.views[i] == nullptr) {
            release_targets(g_targets);
            return false;
        }
    }
    g_targets.width = half_width;
    g_targets.height = half_height;
    return true;
}

WGPUBindGroupEntry texture_entry(uint32_t binding, WGPUTextureView view) {
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = binding;
    entry.textureView = view;
    return entry;
}

WGPUBindGroupEntry uniform_entry(const GfxComputeContext& context, GfxRange range) {
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = 0;
    entry.buffer = context.uniform_buffer;
    entry.offset = range.offset;
    entry.size = range.size;
    return entry;
}

WGPUBindGroup make_group(WGPUDevice device, WGPUBindGroupLayout layout,
    std::initializer_list<WGPUBindGroupEntry> entries) {
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = layout;
    desc.entryCount = entries.size();
    desc.entries = entries.begin();
    return wgpuDeviceCreateBindGroup(device, &desc);
}

}  // namespace

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& trace_source, const ResourceBuffer& temporal_source,
    const ResourceBuffer& atrous_source) {
    g_device_info = device_info;
    return build_pipeline("lighting SSGI trace", "trace_ssgi", common_source, trace_source,
               g_trace_pipeline, g_trace_layout) &&
           build_pipeline("lighting SSGI temporal", "filter_temporal", common_source,
               temporal_source, g_temporal_pipeline, g_temporal_layout) &&
           build_pipeline("lighting SSGI atrous step 1", "filter_step1", common_source,
               atrous_source, g_atrous_pipelines[0], g_atrous_layouts[0]) &&
           build_pipeline("lighting SSGI atrous step 2", "filter_step2", common_source,
               atrous_source, g_atrous_pipelines[1], g_atrous_layouts[1]) &&
           build_pipeline("lighting SSGI atrous step 4", "filter_step4", common_source,
               atrous_source, g_atrous_pipelines[2], g_atrous_layouts[2]);
}

bool prepare_frame(
    uint32_t width, uint32_t height, uint32_t frame_index, uint32_t gi_mode, FrameViews& views) {
    if (g_targets.width != 0 &&
        (!g_have_last_frame || frame_index != g_last_frame + 1u || gi_mode != g_last_mode))
    {
        g_retired.push_back({std::exchange(g_targets, Targets{}), 4});
    }
    if (!ensure_targets(width, height))
        return false;
    const uint32_t write = frame_index & 1u;
    const uint32_t read = write ^ 1u;
    views = {g_targets.views[0], g_targets.views[1 + read], g_targets.views[1 + write],
        g_targets.views[3], g_targets.views[4]};
    g_last_frame = frame_index;
    g_last_mode = gi_mode;
    g_have_last_frame = true;
    return true;
}

bool record(WGPUComputePassEncoder pass, const GfxComputeContext& context, GfxRange uniform_range,
    WGPUTextureView scene_color, WGPUTextureView scene_depth, WGPUTextureView normals,
    WGPUTextureView light_hdr, const FrameViews& views, uint32_t width, uint32_t height,
    bool run_trace) {
    if (pass == nullptr || scene_color == nullptr || scene_depth == nullptr || normals == nullptr ||
        light_hdr == nullptr || views.raw == nullptr || views.history_read == nullptr ||
        views.history_write == nullptr || views.filter_a == nullptr || views.filter_b == nullptr)
    {
        return false;
    }
    const WGPUBindGroupEntry uniform = uniform_entry(context, uniform_range);
    WGPUBindGroup trace = make_group(context.device, g_trace_layout,
        {uniform, texture_entry(1, scene_color), texture_entry(2, scene_depth),
            texture_entry(3, normals), texture_entry(4, light_hdr), texture_entry(5, views.raw)});
    WGPUBindGroup temporal = make_group(context.device, g_temporal_layout,
        {uniform, texture_entry(1, views.raw), texture_entry(2, views.history_read),
            texture_entry(3, scene_depth), texture_entry(4, views.history_write)});
    WGPUBindGroup atrous[3] = {
        make_group(context.device, g_atrous_layouts[0],
            {uniform, texture_entry(1, views.history_write), texture_entry(2, scene_depth),
                texture_entry(3, normals), texture_entry(4, views.filter_a)}),
        make_group(context.device, g_atrous_layouts[1],
            {uniform, texture_entry(1, views.filter_a), texture_entry(2, scene_depth),
                texture_entry(3, normals), texture_entry(4, views.filter_b)}),
        make_group(context.device, g_atrous_layouts[2],
            {uniform, texture_entry(1, views.filter_b), texture_entry(2, scene_depth),
                texture_entry(3, normals), texture_entry(4, views.filter_a)}),
    };
    if (trace == nullptr || temporal == nullptr || atrous[0] == nullptr || atrous[1] == nullptr ||
        atrous[2] == nullptr)
    {
        if (trace != nullptr)
            wgpuBindGroupRelease(trace);
        if (temporal != nullptr)
            wgpuBindGroupRelease(temporal);
        for (WGPUBindGroup group : atrous)
            if (group != nullptr)
                wgpuBindGroupRelease(group);
        return false;
    }

    const uint32_t groups_x = div_ceil(std::max(1u, width / 2u), 8u);
    const uint32_t groups_y = div_ceil(std::max(1u, height / 2u), 8u);
    if (run_trace) {
        wgpuComputePassEncoderSetPipeline(pass, g_trace_pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, trace, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    }
    wgpuComputePassEncoderSetPipeline(pass, g_temporal_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, temporal, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    for (size_t i = 0; i < std::size(atrous); ++i) {
        wgpuComputePassEncoderSetPipeline(pass, g_atrous_pipelines[i]);
        wgpuComputePassEncoderSetBindGroup(pass, 0, atrous[i], 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups_x, groups_y, 1);
    }
    wgpuBindGroupRelease(trace);
    wgpuBindGroupRelease(temporal);
    for (WGPUBindGroup group : atrous)
        wgpuBindGroupRelease(group);
    return true;
}

void tick_retired_targets() {
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (--it->frames_left <= 0) {
            release_targets(it->targets);
            it = g_retired.erase(it);
        } else {
            ++it;
        }
    }
}

void shutdown() {
    release_targets(g_targets);
    for (RetiredTargets& retired : g_retired)
        release_targets(retired.targets);
    g_retired.clear();
    if (g_trace_pipeline != nullptr)
        wgpuComputePipelineRelease(g_trace_pipeline);
    if (g_temporal_pipeline != nullptr)
        wgpuComputePipelineRelease(g_temporal_pipeline);
    if (g_trace_layout != nullptr)
        wgpuBindGroupLayoutRelease(g_trace_layout);
    if (g_temporal_layout != nullptr)
        wgpuBindGroupLayoutRelease(g_temporal_layout);
    g_trace_pipeline = nullptr;
    g_temporal_pipeline = nullptr;
    g_trace_layout = nullptr;
    g_temporal_layout = nullptr;
    for (size_t i = 0; i < std::size(g_atrous_pipelines); ++i) {
        if (g_atrous_pipelines[i] != nullptr)
            wgpuComputePipelineRelease(g_atrous_pipelines[i]);
        if (g_atrous_layouts[i] != nullptr)
            wgpuBindGroupLayoutRelease(g_atrous_layouts[i]);
        g_atrous_pipelines[i] = nullptr;
        g_atrous_layouts[i] = nullptr;
    }
    g_device_info = GFX_DEVICE_INFO_INIT;
    g_last_frame = 0;
    g_last_mode = 0;
    g_have_last_frame = false;
}

}  // namespace lighting::ssgi
