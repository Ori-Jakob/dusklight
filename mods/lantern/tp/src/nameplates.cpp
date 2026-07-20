#include "nameplates.hpp"

#include <mods/svc/gfx.h>
#include <mods/svc/resource.h>

#include <webgpu/webgpu.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <type_traits>
#include <vector>

namespace lantern::tp::nameplates {
namespace {

constexpr uint32_t kAtlasColumns = 8;
constexpr uint32_t kAtlasRows = 6;
constexpr uint32_t kCellWidth = 6;
constexpr uint32_t kCellHeight = 8;
constexpr uint32_t kAtlasWidth = kAtlasColumns * kCellWidth;
constexpr uint32_t kAtlasHeight = kAtlasRows * kCellHeight;
constexpr size_t kMaxNameGlyphs = 48;

// Original compact 5x7 ASCII glyphs. Each low five bits describe one row in the bundled atlas.
constexpr std::array<std::array<uint8_t, 7>, 48> kGlyphRows{{
    {{0, 0, 0, 0, 0, 0, 0}},         // space
    {{14, 17, 1, 2, 4, 0, 4}},       // ?
    {{14, 17, 19, 21, 25, 17, 14}},  // 0
    {{4, 12, 4, 4, 4, 4, 14}},       // 1
    {{14, 17, 1, 2, 4, 8, 31}},      // 2
    {{30, 1, 1, 14, 1, 1, 30}},      // 3
    {{2, 6, 10, 18, 31, 2, 2}},      // 4
    {{31, 16, 16, 30, 1, 1, 30}},    // 5
    {{14, 16, 16, 30, 17, 17, 14}},  // 6
    {{31, 1, 2, 4, 8, 8, 8}},        // 7
    {{14, 17, 17, 14, 17, 17, 14}},  // 8
    {{14, 17, 17, 15, 1, 1, 14}},    // 9
    {{14, 17, 17, 31, 17, 17, 17}},  // A
    {{30, 17, 17, 30, 17, 17, 30}},  // B
    {{14, 17, 16, 16, 16, 17, 14}},  // C
    {{30, 17, 17, 17, 17, 17, 30}},  // D
    {{31, 16, 16, 30, 16, 16, 31}},  // E
    {{31, 16, 16, 30, 16, 16, 16}},  // F
    {{14, 17, 16, 23, 17, 17, 15}},  // G
    {{17, 17, 17, 31, 17, 17, 17}},  // H
    {{14, 4, 4, 4, 4, 4, 14}},       // I
    {{7, 2, 2, 2, 2, 18, 12}},       // J
    {{17, 18, 20, 24, 20, 18, 17}},  // K
    {{16, 16, 16, 16, 16, 16, 31}},  // L
    {{17, 27, 21, 21, 17, 17, 17}},  // M
    {{17, 25, 21, 19, 17, 17, 17}},  // N
    {{14, 17, 17, 17, 17, 17, 14}},  // O
    {{30, 17, 17, 30, 16, 16, 16}},  // P
    {{14, 17, 17, 17, 21, 18, 13}},  // Q
    {{30, 17, 17, 30, 20, 18, 17}},  // R
    {{15, 16, 16, 14, 1, 1, 30}},    // S
    {{31, 4, 4, 4, 4, 4, 4}},        // T
    {{17, 17, 17, 17, 17, 17, 14}},  // U
    {{17, 17, 17, 17, 17, 10, 4}},   // V
    {{17, 17, 17, 21, 21, 21, 10}},  // W
    {{17, 17, 10, 4, 10, 17, 17}},   // X
    {{17, 17, 10, 4, 4, 4, 4}},      // Y
    {{31, 1, 2, 4, 8, 16, 31}},      // Z
    {{0, 0, 0, 31, 0, 0, 0}},        // -
    {{0, 0, 0, 0, 0, 0, 31}},        // _
    {{0, 0, 0, 0, 0, 12, 12}},       // .
    {{4, 4, 2, 0, 0, 0, 0}},         // '
    {{10, 31, 10, 10, 31, 10, 0}},   // #
    {{14, 17, 23, 21, 23, 16, 14}},  // @
    {{0, 4, 4, 31, 4, 4, 0}},        // +
    {{0, 12, 12, 0, 12, 12, 0}},     // :
    {{1, 2, 2, 4, 8, 8, 16}},        // /
    {{4, 4, 4, 4, 4, 0, 4}},         // !
}};

struct alignas(16) LabelUniforms {
    float anchor_and_cell[4];
    float color[4];
    uint32_t glyph_count;
    uint32_t padding[3];
};
static_assert(sizeof(LabelUniforms) == 48);

struct DrawPayload {
    uint32_t uniform_offset;
    uint32_t uniform_size;
    uint32_t glyph_offset;
    uint32_t glyph_size;
    uint32_t glyph_count;
};
static_assert(sizeof(DrawPayload) <= GFX_INLINE_DRAW_PAYLOAD_SIZE);
static_assert(std::is_trivially_copyable_v<DrawPayload>);

ModContext* g_context = nullptr;
const GfxService* g_gfx = nullptr;
GfxDeviceInfo g_device = GFX_DEVICE_INFO_INIT;
GfxDrawTypeHandle g_draw_type = 0;
WGPURenderPipeline g_pipeline = nullptr;
WGPUBindGroupLayout g_layout = nullptr;
WGPUTexture g_atlas = nullptr;
WGPUTextureView g_atlas_view = nullptr;
WGPUSampler g_sampler = nullptr;

uint32_t glyph_index(char input) {
    const unsigned char raw = static_cast<unsigned char>(input);
    const char value = static_cast<char>(std::toupper(raw));
    if (value == ' ')
        return 0;
    if (value >= '0' && value <= '9')
        return 2u + static_cast<uint32_t>(value - '0');
    if (value >= 'A' && value <= 'Z')
        return 12u + static_cast<uint32_t>(value - 'A');
    switch (value) {
    case '-':
        return 38;
    case '_':
        return 39;
    case '.':
        return 40;
    case '\'':
        return 41;
    case '#':
        return 42;
    case '@':
        return 43;
    case '+':
        return 44;
    case ':':
        return 45;
    case '/':
        return 46;
    case '!':
        return 47;
    default:
        return 1;
    }
}

void release_gpu_objects() {
    if (g_sampler != nullptr)
        wgpuSamplerRelease(g_sampler);
    if (g_atlas_view != nullptr)
        wgpuTextureViewRelease(g_atlas_view);
    if (g_atlas != nullptr)
        wgpuTextureRelease(g_atlas);
    if (g_layout != nullptr)
        wgpuBindGroupLayoutRelease(g_layout);
    if (g_pipeline != nullptr)
        wgpuRenderPipelineRelease(g_pipeline);
    g_sampler = nullptr;
    g_atlas_view = nullptr;
    g_atlas = nullptr;
    g_layout = nullptr;
    g_pipeline = nullptr;
}

bool build_atlas() {
    std::array<uint8_t, kAtlasWidth * kAtlasHeight> pixels{};
    for (uint32_t glyph = 0; glyph < kGlyphRows.size(); ++glyph) {
        const uint32_t cell_x = (glyph % kAtlasColumns) * kCellWidth;
        const uint32_t cell_y = (glyph / kAtlasColumns) * kCellHeight;
        for (uint32_t row = 0; row < 7; ++row) {
            for (uint32_t column = 0; column < 5; ++column) {
                if ((kGlyphRows[glyph][row] & (1u << (4u - column))) != 0) {
                    pixels[(cell_y + row) * kAtlasWidth + cell_x + column] = 255;
                }
            }
        }
    }

    WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texture_desc.label = {"Lantern nameplate font atlas", WGPU_STRLEN};
    texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture_desc.size = {kAtlasWidth, kAtlasHeight, 1};
    texture_desc.format = WGPUTextureFormat_R8Unorm;
    g_atlas = wgpuDeviceCreateTexture(g_device.device, &texture_desc);
    if (g_atlas == nullptr)
        return false;
    g_atlas_view = wgpuTextureCreateView(g_atlas, nullptr);
    if (g_atlas_view == nullptr)
        return false;

    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = g_atlas;
    WGPUTexelCopyBufferLayout layout{
        .offset = 0, .bytesPerRow = kAtlasWidth, .rowsPerImage = kAtlasHeight};
    WGPUExtent3D extent{kAtlasWidth, kAtlasHeight, 1};
    wgpuQueueWriteTexture(
        g_device.queue, &destination, pixels.data(), pixels.size(), &layout, &extent);

    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.label = {"Lantern nameplate sampler", WGPU_STRLEN};
    sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_desc.magFilter = WGPUFilterMode_Nearest;
    sampler_desc.minFilter = WGPUFilterMode_Nearest;
    g_sampler = wgpuDeviceCreateSampler(g_device.device, &sampler_desc);
    return g_sampler != nullptr;
}

bool build_pipeline(const ResourceBuffer& source) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {static_cast<const char*>(source.data), source.size};
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = {"Lantern nameplates", WGPU_STRLEN};
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g_device.device, &module_desc);
    if (module == nullptr)
        return false;

    WGPUBlendState blend{
        .color = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_SrcAlpha,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add,
            .srcFactor = WGPUBlendFactor_One,
            .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
    };
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = g_device.color_format;
    target.blend = &blend;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = {"fs_main", WGPU_STRLEN};
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = g_device.depth_format;
    depth.depthWriteEnabled = WGPUOptionalBool_False;
    depth.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.label = {"Lantern nameplates", WGPU_STRLEN};
    pipeline_desc.vertex.module = module;
    pipeline_desc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.depthStencil = &depth;
    pipeline_desc.multisample.count = g_device.sample_count;
    pipeline_desc.fragment = &fragment;
    g_pipeline = wgpuDeviceCreateRenderPipeline(g_device.device, &pipeline_desc);
    wgpuShaderModuleRelease(module);
    if (g_pipeline == nullptr)
        return false;
    g_layout = wgpuRenderPipelineGetBindGroupLayout(g_pipeline, 0);
    return g_layout != nullptr;
}

void on_draw(
    ModContext*, const GfxDrawContext* draw, const void* payload, size_t payload_size, void*) {
    if (payload_size != sizeof(DrawPayload) || draw == nullptr || g_pipeline == nullptr ||
        g_layout == nullptr || g_atlas_view == nullptr || g_sampler == nullptr)
        return;
    DrawPayload data{};
    std::memcpy(&data, payload, sizeof(data));
    if (data.glyph_count == 0 || data.glyph_count > kMaxNameGlyphs)
        return;

    std::array<WGPUBindGroupEntry, 4> entries{WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].buffer = draw->uniform_buffer;
    entries[0].offset = data.uniform_offset;
    entries[0].size = data.uniform_size;
    entries[1].binding = 1;
    entries[1].buffer = draw->storage_buffer;
    entries[1].offset = data.glyph_offset;
    entries[1].size = data.glyph_size;
    entries[2].binding = 2;
    entries[2].textureView = g_atlas_view;
    entries[3].binding = 3;
    entries[3].sampler = g_sampler;
    WGPUBindGroupDescriptor bind_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bind_desc.layout = g_layout;
    bind_desc.entryCount = entries.size();
    bind_desc.entries = entries.data();
    WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(draw->device, &bind_desc);
    if (bind_group == nullptr)
        return;

    wgpuRenderPassEncoderSetPipeline(draw->pass, g_pipeline);
    wgpuRenderPassEncoderSetBindGroup(draw->pass, 0, bind_group, 0, nullptr);
    wgpuRenderPassEncoderDraw(draw->pass, 6, data.glyph_count, 0, 0);
    wgpuBindGroupRelease(bind_group);
}

}  // namespace

bool initialize(ModContext* context, const GfxService* gfx, const ResourceService* resources) {
    g_context = context;
    g_gfx = gfx;
    if (g_gfx == nullptr || resources == nullptr ||
        g_gfx->get_device_info(g_context, &g_device) != MOD_OK)
        return false;

    ResourceBuffer shader = RESOURCE_BUFFER_INIT;
    if (resources->load(g_context, "nameplate.wgsl", &shader) != MOD_OK)
        return false;
    const bool ready = build_atlas() && build_pipeline(shader);
    resources->free(g_context, &shader);
    if (!ready) {
        release_gpu_objects();
        return false;
    }

    GfxDrawTypeDesc desc = GFX_DRAW_TYPE_DESC_INIT;
    desc.label = "Lantern nameplates";
    desc.draw = on_draw;
    if (g_gfx->register_draw_type(g_context, &desc, &g_draw_type) != MOD_OK) {
        release_gpu_objects();
        return false;
    }
    return true;
}

void shutdown() {
    if (g_gfx != nullptr && g_context != nullptr && g_draw_type != 0) {
        g_gfx->unregister_draw_type(g_context, g_draw_type);
    }
    g_draw_type = 0;
    release_gpu_objects();
    g_context = nullptr;
    g_gfx = nullptr;
}

void submit(std::string_view name, uint32_t color_rgb, float normalized_x, float normalized_y) {
    if (g_gfx == nullptr || g_draw_type == 0 || name.empty())
        return;
    std::vector<uint32_t> glyphs;
    glyphs.reserve(std::min(name.size(), kMaxNameGlyphs));
    for (char value : name.substr(0, kMaxNameGlyphs))
        glyphs.push_back(glyph_index(value));
    if (glyphs.empty())
        return;

    LabelUniforms uniforms{};
    uniforms.anchor_and_cell[0] = normalized_x;
    uniforms.anchor_and_cell[1] = normalized_y;
    uniforms.anchor_and_cell[2] = 9.0f / 608.0f;
    uniforms.anchor_and_cell[3] = 12.0f / 448.0f;
    uniforms.color[0] = static_cast<float>((color_rgb >> 16) & 0xFF) / 255.0f;
    uniforms.color[1] = static_cast<float>((color_rgb >> 8) & 0xFF) / 255.0f;
    uniforms.color[2] = static_cast<float>(color_rgb & 0xFF) / 255.0f;
    uniforms.color[3] = 1.0f;
    uniforms.glyph_count = static_cast<uint32_t>(glyphs.size());

    GfxRange uniform_range{};
    GfxRange glyph_range{};
    if (g_gfx->push_uniform(g_context, &uniforms, sizeof(uniforms), &uniform_range) != MOD_OK ||
        g_gfx->push_storage(
            g_context, glyphs.data(), glyphs.size() * sizeof(uint32_t), &glyph_range) != MOD_OK)
        return;
    const DrawPayload payload{uniform_range.offset, uniform_range.size, glyph_range.offset,
        glyph_range.size, static_cast<uint32_t>(glyphs.size())};
    g_gfx->push_draw(g_context, g_draw_type, &payload, sizeof(payload));
}

}  // namespace lantern::tp::nameplates
