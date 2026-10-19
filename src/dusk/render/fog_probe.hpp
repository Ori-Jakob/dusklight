#pragma once

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>

namespace dusk::render {

void fog_probe_env(
    GXFogType type, f32 startZ, f32 endZ, f32 nearZ, f32 farZ, GXColor color) noexcept;
void fog_probe_range(u8 enable, u16 center, const GXFogAdjTable& table) noexcept;

}  // namespace dusk::render
