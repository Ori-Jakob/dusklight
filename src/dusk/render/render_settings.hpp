#pragma once

#include "dusk/render/render_bridge.hpp"
#include "dusk/settings.h"

#include <aurora/aurora.h>
#include <aurora/render.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::render {

struct PresetValues {
    bool stochasticTiling;
    bool generateMissingMips;
    bool taa;
    GraphicsQuality ao;
    bool pbr;
    GraphicsQuality shadowQuality;
    int shadowPages;
    int shadowPageBudget;
    GraphicsGameShadows gameShadows;
    bool ssil;
    GraphicsGi gi;
    GraphicsReflections reflections;
    GraphicsRtMode rt;
    GraphicsQuality fogVolumetric;
    GraphicsGodRays godRays;
    GraphicsQuality clouds;
    bool cloudShadows;
    GraphicsQuality grassQuality;
    int grassDistanceM;
    float grassDistantDensity;
    GraphicsGrassShadows grassShadows;
    GraphicsTerrainDetail terrainDetail;
    GraphicsQuality terrainQuality;
    bool gpuFoliage;
    bool gpuWeather;
    bool gpuParticles;
    bool gpuDeform;

    bool operator==(const PresetValues&) const = default;
};

struct RecommendInput {
    AuroraBackend backend;
    bool core;
    AuroraFeatureProfile profile;
    aurora::render::AdapterKind adapter;
    uint64_t vramBytes;
};

struct RowGate {
    aurora::render::FeatureSupport (*support)(uint8_t option);
    bool (*ready)();
};

struct RowStatus {
    bool locked = false;
    std::string valueLabel;
    std::string reason;
};

const PresetValues& preset_values(GraphicsPreset preset) noexcept;
PresetValues read_preset_values(SettingsSource source = SettingsSource::Current);
void apply_preset(GraphicsPreset preset);
void reset_to_preset();
std::optional<bool> seed_generate_missing_mips();
GraphicsPreset base_preset();
GraphicsPreset detect_preset();
std::string preset_display_name();
GraphicsPreset recommend(const RecommendInput& input) noexcept;
GraphicsPreset recommended_preset();

template <typename T>
T reference_value(const config::ConfigVar<T>& var);
bool is_preset_controlled(std::string_view key) noexcept;
bool differs_from_reference();

bool is_hard_reason(aurora::render::Reason reason) noexcept;
std::string_view reason_label(aurora::render::Reason reason) noexcept;
std::string_view reason_option_label(aurora::render::Reason reason) noexcept;
std::string_view reason_help(aurora::render::Reason reason) noexcept;

template <aurora::render::Feature F>
aurora::render::FeatureSupport feature_gate(uint8_t option) noexcept {
    return aurora::render::feature_support(F, option);
}
template <aurora::render::Feature F>
bool feature_ready_gate() noexcept {
    return aurora::render::feature_ready(F);
}
aurora::render::FeatureSupport always_gate(uint8_t option) noexcept;
aurora::render::FeatureSupport not_built_gate(uint8_t option) noexcept;
aurora::render::FeatureSupport terrain_detail_gate(uint8_t option) noexcept;
aurora::render::FeatureSupport rt_backend_gate(uint8_t option) noexcept;
aurora::render::FeatureSupport native_reenable_gate(uint8_t option) noexcept;
uint8_t game_shadows_option(GraphicsGameShadows value) noexcept;
bool terrain_ready_gate() noexcept;

RowStatus row_status(aurora::render::FeatureSupport row, aurora::render::FeatureSupport option,
    std::string_view requested, std::string_view clamped, bool pending, bool notReady);
RowStatus evaluate_row(const RowGate& gate, uint8_t requestedOption, std::string_view requested,
    std::string_view clamped, bool requestedActive);

void settings_self_test(SelfTestReport& report);

}  // namespace dusk::render
