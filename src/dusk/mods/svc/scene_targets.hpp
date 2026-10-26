#pragma once

#include "mods/svc/scene_targets.h"

namespace dusk::mods::svc {

ModResult gfx_resolve_with_records(ModContext* context, const GfxResolveDesc* desc,
    GfxResolvedTargets* outTargets, const SceneRecordsDesc* recordsDesc, SceneRecords* outRecords);

}  // namespace dusk::mods::svc
