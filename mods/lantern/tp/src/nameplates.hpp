#pragma once

#include <cstdint>
#include <string_view>

struct GfxService;
struct ModContext;
struct ResourceService;

namespace lantern::tp::nameplates {

bool initialize(ModContext* context, const GfxService* gfx, const ResourceService* resources);
void shutdown();
void submit(std::string_view name, uint32_t color_rgb, float normalized_x, float normalized_y);

}  // namespace lantern::tp::nameplates
