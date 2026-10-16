#pragma once

#include "dolphin/types.h"

#include <span>

namespace dusk::render {
struct SelfTestReport;
}

namespace dusk::ui {

class Window;
class Pane;

enum class GraphicsSection : u8 {
    Preset,
    Materials,
    Lighting,
    Shadows,
    AoGi,
    Reflections,
    RayTracing,
    Atmosphere,
    Sky,
    Grass,
    Terrain,
    AntiAliasing,
    PostProcessing,
    Performance,
    Advanced,
};

struct GraphicsRowInfo {
    GraphicsSection section;
    const char* label;
    const char* key;
    const char* unlockedIn;
    bool presetControlled;
};

void build_graphics_tab(Window& window, Pane& leftPane, Pane& rightPane, bool prelaunch);
std::span<const GraphicsRowInfo> graphics_rows() noexcept;
void graphics_tab_self_test(render::SelfTestReport& report);

}  // namespace dusk::ui
