#pragma once

#include "dusk/settings.h"

#include <aurora/render.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class camera_process_class;

namespace dusk::render {

enum class SettingsSource : u8 {
    Current,
    Defaults,
};

struct FieldText {
    std::string_view key;
    std::string requested;
    std::string clamped;
    std::string effective;
    std::string sent;

    bool differs() const noexcept { return sent != clamped || clamped != effective; }
};

struct SelfTestReport {
    size_t passed = 0;
    std::vector<std::string> skipped;
    std::vector<std::string> failed;
};

aurora::render::Preset to_aurora(GraphicsPreset value) noexcept;
aurora::render::Quality to_aurora(GraphicsQuality value) noexcept;
aurora::render::Exposure to_aurora(GraphicsExposure value) noexcept;
aurora::render::Tonemap to_aurora(GraphicsTonemap value) noexcept;
aurora::render::GiMode to_aurora(GraphicsGi value) noexcept;
aurora::render::RtMode to_aurora(GraphicsRtMode value) noexcept;
aurora::render::RtBackend to_aurora(GraphicsRtBackend value) noexcept;
aurora::render::Reflections to_aurora(GraphicsReflections value) noexcept;
aurora::render::GameShadows to_aurora(GraphicsGameShadows value) noexcept;
aurora::render::GodRays to_aurora(GraphicsGodRays value) noexcept;
aurora::render::TerrainDetail to_aurora(GraphicsTerrainDetail value) noexcept;
aurora::render::GrassShadows to_aurora(GraphicsGrassShadows value) noexcept;
aurora::render::StaticWorld to_aurora(GraphicsStaticWorld value) noexcept;

GraphicsPreset from_aurora(aurora::render::Preset value) noexcept;
GraphicsQuality from_aurora(aurora::render::Quality value) noexcept;
GraphicsExposure from_aurora(aurora::render::Exposure value) noexcept;
GraphicsTonemap from_aurora(aurora::render::Tonemap value) noexcept;
GraphicsGi from_aurora(aurora::render::GiMode value) noexcept;
GraphicsRtMode from_aurora(aurora::render::RtMode value) noexcept;
GraphicsRtBackend from_aurora(aurora::render::RtBackend value) noexcept;
GraphicsReflections from_aurora(aurora::render::Reflections value) noexcept;
GraphicsGameShadows from_aurora(aurora::render::GameShadows value) noexcept;
GraphicsGodRays from_aurora(aurora::render::GodRays value) noexcept;
GraphicsTerrainDetail from_aurora(aurora::render::TerrainDetail value) noexcept;
GraphicsGrassShadows from_aurora(aurora::render::GrassShadows value) noexcept;
GraphicsStaticWorld from_aurora(aurora::render::StaticWorld value) noexcept;

void initialize();
void present();
void push_now();
void mark_dirty() noexcept;
bool is_pending() noexcept;

aurora::render::Settings build_requested(SettingsSource source = SettingsSource::Current);
const aurora::render::Settings& last_requested() noexcept;
const aurora::render::Settings& last_clamped() noexcept;
uint64_t push_count() noexcept;
uint32_t last_latch_latency() noexcept;
bool same_settings(const aurora::render::Settings& a, const aurora::render::Settings& b) noexcept;

std::vector<FieldText> describe_fields();
std::string cvar_arguments();

std::string_view preset_name(GraphicsPreset preset) noexcept;
std::span<const aurora::render::Feature> all_features() noexcept;
std::string_view feature_name(aurora::render::Feature feature) noexcept;
std::string_view reason_name(aurora::render::Reason reason) noexcept;

void bridge_self_test(SelfTestReport& report);

struct HintStats {
    uint64_t frames = 0;
    uint64_t sceneFrames = 0;
    uint64_t pausedSceneFrames = 0;
    uint64_t hudOnlyFrames = 0;
    uint64_t otherFrames = 0;
    uint64_t mainCameraHints = 0;
    uint64_t stageHints = 0;
    uint64_t refused = 0;
    uint64_t openAtMarker = 0;
    uint8_t lastStages = 0;
};

void begin_painter();
void submit_main_camera(const camera_process_class& camera, const Mtx view, const Mtx44 proj);
void mark_stage(aurora::render::Stage stage);
void end_scene();
bool has_main_camera() noexcept;
uint32_t current_cut_epoch() noexcept;
const HintStats& hint_stats() noexcept;
void reset_hint_stats() noexcept;
std::vector<std::string> describe_hints();
std::vector<std::string> describe_hint_ids();
void hints_self_test(SelfTestReport& report);

}  // namespace dusk::render
