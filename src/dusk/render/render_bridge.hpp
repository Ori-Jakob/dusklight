#pragma once

#include "dusk/settings.h"

#include <aurora/render.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    bool differs() const noexcept { return requested != clamped || clamped != effective; }
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

}  // namespace dusk::render
