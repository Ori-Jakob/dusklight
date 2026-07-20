#include "gpu.hpp"
#include "ssgi.hpp"
#include "vct.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <webgpu/webgpu.h>

namespace lighting::gpu {
namespace {

GfxDeviceInfo g_device_info = GFX_DEVICE_INFO_INIT;
WGPURenderPipeline g_splat_pipeline = nullptr;
WGPUBindGroupLayout g_splat_layout = nullptr;
WGPURenderPipeline g_composite_pipeline = nullptr;
WGPUBindGroupLayout g_composite_layout = nullptr;
WGPURenderPipeline g_debug_pipeline = nullptr;
WGPUBindGroupLayout g_debug_layout = nullptr;
WGPUComputePipeline g_normals_pipeline = nullptr;
WGPUBindGroupLayout g_normals_layout = nullptr;
WGPUComputePipeline g_cluster_pipeline = nullptr;
WGPUBindGroupLayout g_cluster_layout = nullptr;
WGPUComputePipeline g_shade_pipeline = nullptr;
WGPUBindGroupLayout g_shade_layout = nullptr;
WGPUTexture g_gi_fallback = nullptr;
WGPUTextureView g_gi_fallback_view = nullptr;

struct Targets {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t shade_width = 0;
    uint32_t shade_height = 0;
    bool owns_normals = false;
    WGPUTexture normals = nullptr;
    WGPUTextureView normals_view = nullptr;
    WGPUTexture light_hdr = nullptr;
    WGPUTextureView light_hdr_view = nullptr;
    WGPUBuffer clusters = nullptr;
};
Targets g_targets;
struct RetiredTargets {
    Targets targets;
    int frames_left = 0;
};
std::vector<RetiredTargets> g_retired_targets;

std::atomic<bool> g_first_draw{false};
std::atomic<bool> g_first_compute{false};

constexpr uint32_t div_ceil(uint32_t numerator, uint32_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

WGPUShaderModule create_shader_module(
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

bool build_compute(const char* label, const ResourceBuffer& common, const ResourceBuffer& source,
    const char* entry, WGPUComputePipeline& pipeline, WGPUBindGroupLayout& layout) {
    WGPUShaderModule module = create_shader_module(label, common, source);
    if (module == nullptr) {
        return false;
    }
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {label, WGPU_STRLEN};
    desc.compute.module = module;
    desc.compute.entryPoint = {entry, WGPU_STRLEN};
    pipeline = wgpuDeviceCreateComputePipeline(g_device_info.device, &desc);
    wgpuShaderModuleRelease(module);
    if (pipeline == nullptr) {
        return false;
    }
    layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    return layout != nullptr;
}

bool build_splat(const ResourceBuffer& common, const ResourceBuffer& source) {
    WGPUShaderModule module = create_shader_module("lighting splat debug", common, source);
    if (module == nullptr) {
        return false;
    }
    WGPUBlendState blend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_SrcAlpha,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = g_device_info.color_format;
    target.blend = &blend;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_splat", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = g_device_info.depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {"lighting light-splat debug", WGPU_STRLEN};
    desc.vertex.module = module;
    desc.vertex.entryPoint = {"vs_splat", WGPU_STRLEN};
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.depthStencil = &depth;
    desc.multisample.count = g_device_info.sample_count;
    desc.fragment = &fragment;
    g_splat_pipeline = wgpuDeviceCreateRenderPipeline(g_device_info.device, &desc);
    wgpuShaderModuleRelease(module);
    if (g_splat_pipeline == nullptr) {
        return false;
    }
    g_splat_layout = wgpuRenderPipelineGetBindGroupLayout(g_splat_pipeline, 0);
    return g_splat_layout != nullptr;
}

bool build_composite(const ResourceBuffer& common, const ResourceBuffer& source, bool debug,
    WGPURenderPipeline& pipeline, WGPUBindGroupLayout& layout) {
    WGPUShaderModule module = create_shader_module("lighting composite", common, source);
    if (module == nullptr) {
        return false;
    }
    WGPUBlendState blend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_One,
            .dstFactor = WGPUBlendFactor_One},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_Zero,
            .dstFactor = WGPUBlendFactor_One},
    };
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = g_device_info.color_format;
    if (!debug) {
        target.blend = &blend;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {debug ? "fs_debug" : "fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = g_device_info.depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = {debug ? "lighting composite (debug)" : "lighting composite", WGPU_STRLEN};
    desc.vertex.module = module;
    desc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.depthStencil = &depth;
    desc.multisample.count = g_device_info.sample_count;
    desc.fragment = &fragment;
    pipeline = wgpuDeviceCreateRenderPipeline(g_device_info.device, &desc);
    wgpuShaderModuleRelease(module);
    if (pipeline == nullptr) {
        return false;
    }
    layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
    return layout != nullptr;
}

void release_targets(Targets& targets) {
    if (targets.normals_view != nullptr)
        wgpuTextureViewRelease(targets.normals_view);
    if (targets.light_hdr_view != nullptr)
        wgpuTextureViewRelease(targets.light_hdr_view);
    if (targets.normals != nullptr)
        wgpuTextureRelease(targets.normals);
    if (targets.light_hdr != nullptr)
        wgpuTextureRelease(targets.light_hdr);
    if (targets.clusters != nullptr)
        wgpuBufferRelease(targets.clusters);
    targets = {};
}

bool ensure_targets(uint32_t width, uint32_t height, bool half_res, bool need_own_normals) {
    const uint32_t shade_width = half_res ? std::max(1u, width / 2) : width;
    const uint32_t shade_height = half_res ? std::max(1u, height / 2) : height;
    if (g_targets.width == width && g_targets.height == height &&
        g_targets.shade_width == shade_width && g_targets.shade_height == shade_height &&
        g_targets.owns_normals == need_own_normals)
    {
        return true;
    }
    if (g_targets.width != 0) {
        g_retired_targets.push_back({std::exchange(g_targets, Targets{}), 4});
    }
    const auto create_texture = [](const char* label, uint32_t w, uint32_t h, WGPUTexture& texture,
                                    WGPUTextureView& view) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = {label, WGPU_STRLEN};
        desc.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        desc.size = {w, h, 1};
        desc.format = WGPUTextureFormat_RGBA16Float;
        texture = wgpuDeviceCreateTexture(g_device_info.device, &desc);
        if (texture == nullptr)
            return false;
        view = wgpuTextureCreateView(texture, nullptr);
        return view != nullptr;
    };
    bool ok = true;
    if (need_own_normals) {
        ok = create_texture("lighting reconstructed normals", width, height, g_targets.normals,
            g_targets.normals_view);
    }
    if (ok) {
        ok = create_texture("lighting direct HDR", shade_width, shade_height, g_targets.light_hdr,
            g_targets.light_hdr_view);
    }
    if (ok) {
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.label = {"lighting cluster lists", WGPU_STRLEN};
        desc.usage = WGPUBufferUsage_Storage;
        desc.size = static_cast<uint64_t>(kClusterX) * kClusterY * kClusterZ * kClusterStride *
                    sizeof(uint32_t);
        g_targets.clusters = wgpuDeviceCreateBuffer(g_device_info.device, &desc);
        ok = g_targets.clusters != nullptr;
    }
    if (!ok) {
        release_targets(g_targets);
        return false;
    }
    g_targets.width = width;
    g_targets.height = height;
    g_targets.shade_width = shade_width;
    g_targets.shade_height = shade_height;
    g_targets.owns_normals = need_own_normals;
    return true;
}

WGPUBindGroup make_group(WGPUDevice device, WGPUBindGroupLayout layout,
    std::initializer_list<WGPUBindGroupEntry> entries) {
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = layout;
    desc.entryCount = entries.size();
    desc.entries = entries.begin();
    return wgpuDeviceCreateBindGroup(device, &desc);
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

void release_group(WGPUBindGroup group) {
    if (group != nullptr)
        wgpuBindGroupRelease(group);
}

}  // namespace

bool initialize(const GfxDeviceInfo& device_info, const ResourceBuffer& common_source,
    const ResourceBuffer& splat_source, const ResourceBuffer& normals_source,
    const ResourceBuffer& cluster_source, const ResourceBuffer& shade_source,
    const ResourceBuffer& composite_source, const ResourceBuffer& ssgi_trace_source,
    const ResourceBuffer& temporal_source, const ResourceBuffer& atrous_source,
    const ResourceBuffer& vct_inject_source, const ResourceBuffer& vct_resolve_source,
    const ResourceBuffer& vct_trace_source) {
    g_device_info = device_info;
    if (!build_splat(common_source, splat_source) ||
        !build_compute("lighting normals", common_source, normals_source, "reconstruct_normals",
            g_normals_pipeline, g_normals_layout) ||
        !build_compute("lighting cluster cull", common_source, cluster_source, "cull_lights",
            g_cluster_pipeline, g_cluster_layout) ||
        !build_compute("lighting shade", common_source, shade_source, "shade_lights",
            g_shade_pipeline, g_shade_layout) ||
        !build_composite(
            common_source, composite_source, false, g_composite_pipeline, g_composite_layout) ||
        !build_composite(common_source, composite_source, true, g_debug_pipeline, g_debug_layout) ||
        !ssgi::initialize(
            device_info, common_source, ssgi_trace_source, temporal_source, atrous_source) ||
        !vct::initialize(
            device_info, common_source, vct_inject_source, vct_resolve_source, vct_trace_source))
    {
        return false;
    }

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = {"lighting zero GI fallback", WGPU_STRLEN};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.size = {1, 1, 1};
    desc.format = WGPUTextureFormat_RGBA16Float;
    g_gi_fallback = wgpuDeviceCreateTexture(device_info.device, &desc);
    if (g_gi_fallback == nullptr)
        return false;
    g_gi_fallback_view = wgpuTextureCreateView(g_gi_fallback, nullptr);
    if (g_gi_fallback_view == nullptr)
        return false;
    const uint16_t zero[4] = {};
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = g_gi_fallback;
    WGPUTexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = 8, .rowsPerImage = 1};
    const WGPUExtent3D extent{1, 1, 1};
    wgpuQueueWriteTexture(device_info.queue, &dst, zero, sizeof(zero), &layout, &extent);
    return true;
}

void tick_retired_targets() {
    ssgi::tick_retired_targets();
    for (auto it = g_retired_targets.begin(); it != g_retired_targets.end();) {
        if (--it->frames_left <= 0) {
            release_targets(it->targets);
            it = g_retired_targets.erase(it);
        } else {
            ++it;
        }
    }
}

bool prepare_direct_frame(uint32_t width, uint32_t height, bool half_res,
    const GfxResolvedTargets& resolved, GfxRange uniform_range, GfxRange lights_range,
    WGPUTextureView shared_normals, uint32_t light_count, uint32_t debug_view, uint32_t gi_mode,
    uint32_t frame_index, ComputePayload& compute, DrawPayload& draw) {
    if (resolved.color == nullptr || resolved.depth == nullptr || width == 0 || height == 0 ||
        !ensure_targets(width, height, half_res, shared_normals == nullptr))
    {
        return false;
    }
    WGPUTextureView normals = shared_normals != nullptr ? shared_normals : g_targets.normals_view;
    ssgi::FrameViews gi{};
    WGPUTextureView gi_filtered = g_gi_fallback_view;
    if (gi_mode == 1u || gi_mode == 2u) {
        if (!ssgi::prepare_frame(width, height, frame_index, gi_mode, gi))
            return false;
        gi_filtered = gi.filter_a;
    }
    if (gi_mode == 2u && !vct::prepare_frame())
        return false;
    compute = {resolved.color, resolved.depth, normals, g_targets.light_hdr_view, gi.raw,
        gi.history_read, gi.history_write, gi.filter_a, gi.filter_b, g_targets.clusters,
        uniform_range.offset, uniform_range.size, lights_range.offset, lights_range.size, width,
        height, g_targets.shade_width, g_targets.shade_height, shared_normals == nullptr ? 1u : 0u,
        gi_mode, frame_index};
    draw = {resolved.color, resolved.depth, normals, g_targets.light_hdr_view, gi_filtered,
        uniform_range.offset, uniform_range.size, 0, 0, light_count, debug_view, DrawKindComposite};
    return true;
}

DrawPayload make_splat_payload(
    GfxRange uniform_range, GfxRange lights_range, uint32_t light_count, uint32_t debug_view) {
    return {nullptr, nullptr, nullptr, nullptr, nullptr, uniform_range.offset, uniform_range.size,
        lights_range.offset, lights_range.size, light_count, debug_view, DrawKindSplat};
}

void on_compute(
    ModContext*, const GfxComputeContext* ctx, const void* payload, size_t payload_size, void*) {
    if (payload_size != sizeof(ComputePayload) || ctx == nullptr)
        return;
    ComputePayload data{};
    std::memcpy(&data, payload, sizeof(data));
    if (data.scene_depth == nullptr || data.normals == nullptr || data.light_hdr == nullptr ||
        data.clusters == nullptr)
    {
        return;
    }

    const auto uniform =
        buffer_entry(0, ctx->uniform_buffer, data.uniform_offset, data.uniform_size);
    const auto lights = [&](uint32_t binding) {
        return buffer_entry(binding, ctx->storage_buffer, data.lights_offset, data.lights_size);
    };
    WGPUBindGroup normals_group = nullptr;
    if (data.reconstruct_normals != 0u) {
        normals_group = make_group(ctx->device, g_normals_layout,
            {uniform, texture_entry(1, data.scene_depth), texture_entry(2, data.normals)});
    }
    WGPUBindGroup cluster_group = make_group(ctx->device, g_cluster_layout,
        {uniform, lights(1), buffer_entry(2, data.clusters, 0, WGPU_WHOLE_SIZE)});
    WGPUBindGroup shade_group = make_group(ctx->device, g_shade_layout,
        {uniform, texture_entry(1, data.scene_depth), texture_entry(2, data.normals), lights(3),
            buffer_entry(4, data.clusters, 0, WGPU_WHOLE_SIZE), texture_entry(5, data.light_hdr)});
    if ((data.reconstruct_normals != 0u && normals_group == nullptr) || cluster_group == nullptr ||
        shade_group == nullptr)
    {
        release_group(normals_group);
        release_group(cluster_group);
        release_group(shade_group);
        return;
    }

    WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    desc.label = {"clustered deferred direct lighting", WGPU_STRLEN};
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(ctx->encoder, &desc);
    if (data.reconstruct_normals != 0u) {
        wgpuComputePassEncoderSetPipeline(pass, g_normals_pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, normals_group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(
            pass, div_ceil(data.full_width, 8), div_ceil(data.full_height, 8), 1);
    }
    wgpuComputePassEncoderSetPipeline(pass, g_cluster_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, cluster_group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(kClusterX * kClusterY * kClusterZ, 64), 1, 1);
    wgpuComputePassEncoderSetPipeline(pass, g_shade_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, shade_group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, div_ceil(data.shade_width, 8), div_ceil(data.shade_height, 8), 1);
    if (data.gi_mode == 1u) {
        const ssgi::FrameViews gi{data.gi_raw, data.gi_history_read, data.gi_history_write,
            data.gi_filter_a, data.gi_filter_b};
        ssgi::record(pass, *ctx, {data.uniform_offset, data.uniform_size}, data.scene_color,
            data.scene_depth, data.normals, data.light_hdr, gi, data.full_width, data.full_height,
            true);
    } else if (data.gi_mode == 2u) {
        const ssgi::FrameViews gi{data.gi_raw, data.gi_history_read, data.gi_history_write,
            data.gi_filter_a, data.gi_filter_b};
        vct::record(pass, *ctx, {data.uniform_offset, data.uniform_size}, data.scene_color,
            data.scene_depth, data.normals, data.light_hdr, data.gi_raw, data.full_width,
            data.full_height, data.frame_index);
        ssgi::record(pass, *ctx, {data.uniform_offset, data.uniform_size}, data.scene_color,
            data.scene_depth, data.normals, data.light_hdr, gi, data.full_width, data.full_height,
            false);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    release_group(normals_group);
    release_group(cluster_group);
    release_group(shade_group);
    g_first_compute.store(true, std::memory_order_release);
}

void on_draw(
    ModContext*, const GfxDrawContext* ctx, const void* payload, size_t payload_size, void*) {
    if (payload_size != sizeof(DrawPayload) || ctx == nullptr)
        return;
    DrawPayload data{};
    std::memcpy(&data, payload, sizeof(data));
    if (data.kind == DrawKindSplat) {
        if (data.debug_view != 1 || data.light_count == 0 || g_splat_pipeline == nullptr)
            return;
        WGPUBindGroup group = make_group(ctx->device, g_splat_layout,
            {buffer_entry(0, ctx->uniform_buffer, data.uniform_offset, data.uniform_size),
                buffer_entry(1, ctx->storage_buffer, data.lights_offset, data.lights_size)});
        if (group == nullptr)
            return;
        wgpuRenderPassEncoderSetPipeline(ctx->pass, g_splat_pipeline);
        wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderDraw(ctx->pass, 6, data.light_count, 0, 0);
        wgpuBindGroupRelease(group);
        g_first_draw.store(true, std::memory_order_release);
        return;
    }
    if (data.scene_color == nullptr || data.scene_depth == nullptr || data.normals == nullptr ||
        data.light_hdr == nullptr || data.gi_filtered == nullptr)
    {
        return;
    }
    const bool debug = data.debug_view >= 2;
    WGPURenderPipeline pipeline = debug ? g_debug_pipeline : g_composite_pipeline;
    WGPUBindGroupLayout layout = debug ? g_debug_layout : g_composite_layout;
    WGPUBindGroup group = make_group(ctx->device, layout,
        {texture_entry(0, data.scene_color), texture_entry(1, data.scene_depth),
            texture_entry(2, data.normals), texture_entry(3, data.light_hdr),
            texture_entry(4, data.gi_filtered),
            buffer_entry(5, ctx->uniform_buffer, data.uniform_offset, data.uniform_size)});
    if (group == nullptr || pipeline == nullptr) {
        release_group(group);
        return;
    }
    wgpuRenderPassEncoderSetPipeline(ctx->pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(ctx->pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(ctx->pass, 3, 1, 0, 0);
    wgpuBindGroupRelease(group);
    g_first_draw.store(true, std::memory_order_release);
}

bool consume_first_draw() {
    return g_first_draw.exchange(false, std::memory_order_acq_rel);
}

bool consume_first_compute() {
    return g_first_compute.exchange(false, std::memory_order_acq_rel);
}

void shutdown() {
    vct::shutdown();
    ssgi::shutdown();
    release_targets(g_targets);
    for (auto& retired : g_retired_targets)
        release_targets(retired.targets);
    g_retired_targets.clear();
    const auto release_compute = [](WGPUComputePipeline& value) {
        if (value != nullptr)
            wgpuComputePipelineRelease(value);
        value = nullptr;
    };
    const auto release_render = [](WGPURenderPipeline& value) {
        if (value != nullptr)
            wgpuRenderPipelineRelease(value);
        value = nullptr;
    };
    const auto release_layout = [](WGPUBindGroupLayout& value) {
        if (value != nullptr)
            wgpuBindGroupLayoutRelease(value);
        value = nullptr;
    };
    release_compute(g_normals_pipeline);
    release_compute(g_cluster_pipeline);
    release_compute(g_shade_pipeline);
    release_render(g_splat_pipeline);
    release_render(g_composite_pipeline);
    release_render(g_debug_pipeline);
    release_layout(g_normals_layout);
    release_layout(g_cluster_layout);
    release_layout(g_shade_layout);
    release_layout(g_splat_layout);
    release_layout(g_composite_layout);
    release_layout(g_debug_layout);
    if (g_gi_fallback_view != nullptr)
        wgpuTextureViewRelease(g_gi_fallback_view);
    if (g_gi_fallback != nullptr)
        wgpuTextureRelease(g_gi_fallback);
    g_gi_fallback_view = nullptr;
    g_gi_fallback = nullptr;
    g_device_info = GFX_DEVICE_INFO_INIT;
    g_first_draw.store(false, std::memory_order_release);
    g_first_compute.store(false, std::memory_order_release);
}

}  // namespace lighting::gpu
