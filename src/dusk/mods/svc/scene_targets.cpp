#include "scene_targets.hpp"

#include "internal.hpp"
#include "registry.hpp"

#include "dusk/mods/loader/loader.hpp"

#include <aurora/gfx.hpp>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <cstdint>

namespace dusk::mods::svc {
namespace {

static_assert(sizeof(void*) != 8 || sizeof(SceneRecordsDesc) == 8);
static_assert(sizeof(void*) != 8 || sizeof(SceneRecords) == 24);
static_assert(sizeof(void*) != 8 || sizeof(SceneTargetsLayout) == 32);

ModResult scene_targets_get_scene_targets(ModContext* context, SceneTargetsLayout* outLayout) {
    if (outLayout == nullptr || outLayout->struct_size < sizeof(SceneTargetsLayout) ||
        mod_from_context(context) == nullptr)
    {
        return MOD_INVALID_ARGUMENT;
    }
    // Same drain as GfxService's layout query: the layout changes when recording begins
    AuroraGXSync();

    const auto layout = aurora::gfx::scene_render_target_layout();
    const uint32_t structSize = outLayout->struct_size;
    *outLayout = SceneTargetsLayout SCENE_TARGETS_LAYOUT_INIT;
    outLayout->struct_size = structSize;
    outLayout->key = layout.key;
    const uint32_t count =
        std::min<uint32_t>(layout.colorAttachmentCount, GFX_MAX_COLOR_ATTACHMENTS);
    for (uint32_t i = 0; i < count; ++i) {
        switch (layout.colorAttachments[i].semantic) {
        case aurora::gfx::ColorAttachmentSemantic::Motion:
            outLayout->motion = i;
            break;
        case aurora::gfx::ColorAttachmentSemantic::Reactive:
            outLayout->reactive = i;
            break;
        case aurora::gfx::ColorAttachmentSemantic::Fog:
            outLayout->fog = i;
            break;
        default:
            break;
        }
    }
    return MOD_OK;
}

constexpr SceneTargetsService s_sceneTargetsService{
    .header = SERVICE_HEADER(
        SceneTargetsService, SCENE_TARGETS_SERVICE_MAJOR, SCENE_TARGETS_SERVICE_MINOR),
    .get_scene_targets = scene_targets_get_scene_targets,
    .resolve_pass = gfx_resolve_with_records,
};

}  // namespace

constinit const ServiceModule g_sceneTargetsModule{
    .id = SCENE_TARGETS_SERVICE_ID,
    .majorVersion = SCENE_TARGETS_SERVICE_MAJOR,
    .minorVersion = SCENE_TARGETS_SERVICE_MINOR,
    .service = &s_sceneTargetsService,
};

}  // namespace dusk::mods::svc
