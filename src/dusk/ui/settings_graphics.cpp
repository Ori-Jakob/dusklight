#include "settings_graphics.hpp"

#include "bool_button.hpp"
#include "graphics_tuner.hpp"
#include "pane.hpp"
#include "settings_controls.hpp"
#include "ui.hpp"

#include "dusk/config.hpp"
#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/main.h"
#include "dusk/render/graphics_fields.inl"
#include "dusk/render/render_bridge.hpp"
#include "dusk/render/render_settings.hpp"
#include "dusk/settings.h"
#include "dusk/speedrun.h"

#include <aurora/render.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dusk::ui {
namespace {
namespace ar = aurora::render;
using render::RowGate;
using render::RowStatus;

const Rml::String kShadowResolutionHelpText =
    "Configure the shadow-map resolution. Higher values improve shadow quality but increase GPU "
    "and memory usage.";
const Rml::String kBloomHelpText =
    "Configure the post-processing bloom effect. Classic uses the original bloom pass; Dusklight "
    "uses a higher-quality bloom pass.";
const Rml::String kBloomBrightnessHelpText =
    "Configure bloom intensity. Higher values make bright areas glow more strongly.";
const Rml::String kDepthOfFieldHelpText =
    "Configure the post-processing depth-of-field effect. Classic uses the original "
    "depth-of-field pass; Dusklight uses a higher-quality depth-of-field pass.";
const Rml::String kTextureReplacementHelpText = "Enable installed texture replacements.";

constexpr std::array kSectionTitles{
    "Preset",
    "Materials & Textures",
    "Lighting",
    "Shadows",
    "AO & GI",
    "Reflections",
    "Ray Tracing",
    "Atmosphere",
    "Sky & Clouds",
    "Grass",
    "Terrain",
    "Anti-Aliasing",
    "Post-Processing",
    "Performance",
    "Advanced",
};
static_assert(kSectionTitles.size() == static_cast<size_t>(GraphicsSection::Advanced) + 1);

constexpr std::array<size_t, kSectionTitles.size()> kSectionRowCounts{
    3, 6, 4, 5, 3, 2, 3, 6, 11, 10, 5, 2, 3, 6, 4};

constexpr std::array kMovedKeys{
    std::string_view{"game.enableTextureReplacements"},
    std::string_view{"game.shadowResolutionMultiplier"},
    std::string_view{"game.disableWaterRefraction"},
    std::string_view{"game.bloomMode"},
    std::string_view{"game.bloomMultiplier"},
    std::string_view{"game.depthOfFieldMode"},
};

constexpr std::array<EnumOption<GraphicsPreset>, 5> kPresetOptions{{
    {GraphicsPreset::Original, "Original"},
    {GraphicsPreset::Low, "Low"},
    {GraphicsPreset::Medium, "Medium"},
    {GraphicsPreset::High, "High"},
    {GraphicsPreset::Ultra, "Ultra"},
}};

constexpr std::array<EnumOption<GraphicsQuality>, 5> kQualityOptions{{
    {GraphicsQuality::Off, "Off"},
    {GraphicsQuality::Low, "Low"},
    {GraphicsQuality::Medium, "Medium"},
    {GraphicsQuality::High, "High"},
    {GraphicsQuality::Ultra, "Ultra"},
}};

constexpr std::array<EnumOption<GraphicsQuality>, 4> kTessellationOptions{{
    {GraphicsQuality::Low, "Low (8 px)"},
    {GraphicsQuality::Medium, "Medium (6 px)"},
    {GraphicsQuality::High, "High (4 px)"},
    {GraphicsQuality::Ultra, "Ultra (2 px)"},
}};

constexpr std::array<EnumOption<GraphicsExposure>, 3> kExposureOptions{{
    {GraphicsExposure::MatchLegacy, "Match Original"},
    {GraphicsExposure::Auto, "Auto"},
    {GraphicsExposure::Manual, "Manual"},
}};

constexpr std::array<EnumOption<GraphicsTonemap>, 2> kTonemapOptions{{
    {GraphicsTonemap::PbrNeutral, "PBR Neutral"},
    {GraphicsTonemap::AgX, "AgX"},
}};

constexpr std::array<EnumOption<GraphicsGameShadows>, 3> kGameShadowOptions{{
    {GraphicsGameShadows::Keep, "Keep"},
    {GraphicsGameShadows::Auto, "Replace When Covered"},
    {GraphicsGameShadows::Remove, "Remove"},
}};

constexpr std::array<EnumOption<GraphicsGi>, 3> kGiOptions{{
    {GraphicsGi::Off, "Off"},
    {GraphicsGi::Voxel, "Voxel"},
    {GraphicsGi::Ddgi, "DDGI"},
}};

constexpr std::array<EnumOption<GraphicsReflections>, 3> kReflectionOptions{{
    {GraphicsReflections::Off, "Off"},
    {GraphicsReflections::Ssr, "SSR"},
    {GraphicsReflections::SsrRt, "SSR + Ray Tracing"},
}};

constexpr std::array<EnumOption<GraphicsRtMode>, 5> kRtModeOptions{{
    {GraphicsRtMode::Off, "Off"},
    {GraphicsRtMode::Shadows, "Shadows"},
    {GraphicsRtMode::ShadowsAo, "Shadows + AO"},
    {GraphicsRtMode::ShadowsAoReflections, "Shadows + AO + Reflections"},
    {GraphicsRtMode::PathTraceReference, "Path-Traced Reference"},
}};

constexpr std::array<EnumOption<GraphicsRtBackend>, 3> kRtBackendOptions{{
    {GraphicsRtBackend::Auto, "Auto"},
    {GraphicsRtBackend::Compute, "Compute"},
    {GraphicsRtBackend::Dxr, "Native (DXR)"},
}};

constexpr std::array<EnumOption<GraphicsGodRays>, 4> kGodRayOptions{{
    {GraphicsGodRays::Off, "Off"},
    {GraphicsGodRays::Auto, "Auto"},
    {GraphicsGodRays::ScreenSpace, "Screen Space"},
    {GraphicsGodRays::Volumetric, "Volumetric"},
}};

constexpr std::array<EnumOption<GraphicsGrassShadows>, 4> kGrassShadowOptions{{
    {GraphicsGrassShadows::Off, "Off"},
    {GraphicsGrassShadows::Contact, "Contact"},
    {GraphicsGrassShadows::Shell, "Shell"},
    {GraphicsGrassShadows::Full, "Full"},
}};

constexpr std::array<EnumOption<GraphicsTerrainDetail>, 3> kTerrainDetailOptions{{
    {GraphicsTerrainDetail::Off, "Off"},
    {GraphicsTerrainDetail::Parallax, "Parallax"},
    {GraphicsTerrainDetail::Tessellated, "Tessellated"},
}};

constexpr std::array<EnumOption<GraphicsStaticWorld>, 3> kStaticWorldOptions{{
    {GraphicsStaticWorld::Off, "Off"},
    {GraphicsStaticWorld::Validate, "Validate"},
    {GraphicsStaticWorld::On, "On"},
}};

struct RowContext {
    Window& window;
    Pane& left;
    Pane& right;
    bool prelaunch;
};

struct GraphicsRow {
    GraphicsRowInfo info;
    void (*build)(RowContext&, const GraphicsRowInfo&);
};

template <ar::Feature F>
constexpr RowGate toggle_gate() {
    return {&render::feature_gate<F>, &render::feature_ready_gate<F>};
}

template <ar::Feature F>
constexpr RowGate param_gate() {
    return {&render::feature_gate<F>, nullptr};
}

constexpr RowGate kAlwaysGate{&render::always_gate, nullptr};
constexpr RowGate kNotBuiltGate{&render::not_built_gate, nullptr};

auto& graphics() {
    return getSettings().graphics;
}

const ar::Settings& clamped() {
    return render::last_clamped();
}

bool advanced_off() {
    return !getSettings().backend.enableAdvancedSettings.getValue();
}

Rml::String on_off(bool value) {
    return value ? "On" : "Off";
}

template <typename E>
uint8_t enum_option(E value) {
    return static_cast<uint8_t>(value);
}

template <typename E>
Rml::String option_label(std::span<const EnumOption<E>> options, E value) {
    for (const auto& option : options) {
        if (option.value == value) {
            return option.label;
        }
    }
    return "Unknown";
}

// Every widget callback asks for the status, so it is worked out once per push or change
struct RowState {
    RowGate gate;
    std::function<uint8_t()> option;
    std::function<Rml::String()> requested;
    std::function<Rml::String()> clamped;
    std::function<bool()> active;

    bool valid = false;
    uint64_t pushes = 0;
    bool pending = false;
    uint8_t requestedOption = 0;
    Rml::String requestedLabel;
    RowStatus cached;

    const RowStatus& status() {
        const uint64_t nowPushes = render::push_count();
        const bool nowPending = render::is_pending();
        const uint8_t nowOption = option ? option() : 0;
        Rml::String nowRequested = requested ? requested() : Rml::String{};
        if (!valid || nowPushes != pushes || nowPending != pending ||
            nowOption != requestedOption || nowRequested != requestedLabel)
        {
            const Rml::String nowClamped = clamped ? clamped() : nowRequested;
            cached =
                render::evaluate_row(gate, nowOption, nowRequested, nowClamped, active && active());
            valid = true;
            pushes = nowPushes;
            pending = nowPending;
            requestedOption = nowOption;
            requestedLabel = std::move(nowRequested);
        }
        return cached;
    }

    Rml::String unavailable_reason() {
        const RowStatus& s = status();
        return s.locked ? s.reason : Rml::String{};
    }

    Rml::String value_label() { return status().valueLabel; }

    Rml::String help_suffix() {
        const RowStatus& s = status();
        return s.locked ? Rml::String{} : s.reason;
    }
};

struct BoolRowProps {
    const char* help;
    RowGate gate;
    bool (*clamped)();
    std::function<bool()> isDisabled;
};

void bool_row(
    RowContext& ctx, const GraphicsRowInfo& info, ConfigVar<bool>& var, BoolRowProps props) {
    auto state = std::make_shared<RowState>(RowState{
        .gate = props.gate,
        .requested = [&var] { return on_off(var.getValue()); },
        .clamped = [clampedValue = props.clamped] { return on_off(clampedValue()); },
        .active = [&var] { return var.getValue(); },
    });
    config_bool_select(ctx.left, ctx.right, var,
        {
            .key = info.label,
            .helpText = props.help,
            .isDisabled = std::move(props.isDisabled),
            .unavailableReason = [state] { return state->unavailable_reason(); },
            .valueOverride = [state] { return state->value_label(); },
            .isModified = [&var] { return var.getValue() != render::reference_value(var); },
            .helpSuffix = [state] { return state->help_suffix(); },
        });
}

template <typename E>
struct EnumRowProps {
    const char* help;
    std::span<const EnumOption<E>> options;
    RowGate gate;
    E (*clamped)();
    std::function<bool()> isDisabled;
    uint8_t (*option)(E) = &enum_option<E>;
};

template <typename E>
void enum_row(RowContext& ctx, const GraphicsRowInfo& info, ConfigVar<E>& var,
    EnumRowProps<std::type_identity_t<E>> props) {
    const auto options = props.options;
    const auto optionOf = props.option;
    const RowGate gate = props.gate;
    auto state = std::make_shared<RowState>(RowState{
        .gate = gate,
        .option = [&var, optionOf] { return optionOf(var.getValue()); },
        .requested = [&var, options] { return option_label(options, var.getValue()); },
        .clamped = [options, clampedValue =
                                 props.clamped] { return option_label(options, clampedValue()); },
        .active = [&var, optionOf] { return optionOf(var.getValue()) != 0; },
    });
    config_enum_select(ctx.left, ctx.right, var,
        ConfigEnumProps<E>{
            .key = info.label,
            .helpText = props.help,
            .options = std::vector<EnumOption<E>>(options.begin(), options.end()),
            .isDisabled = std::move(props.isDisabled),
            .isModified = [&var] { return var.getValue() != render::reference_value(var); },
            .unavailableReason = [state] { return state->unavailable_reason(); },
            .optionUnavailableReason =
                [gate, optionOf](E value) {
                    const ar::FeatureSupport support = gate.support(optionOf(value));
                    if (support.supported || !render::is_hard_reason(support.reason)) {
                        return Rml::String{};
                    }
                    return Rml::String{render::reason_option_label(support.reason)};
                },
            .valueOverride = [state] { return state->value_label(); },
            .helpSuffix = [state] { return state->help_suffix(); },
        });
}

struct TunerRowProps {
    GraphicsOption option;
    const char* help;
    RowGate gate;
    std::function<bool()> isDisabled;
};

void tuner_row(RowContext& ctx, const GraphicsRowInfo& info, TunerRowProps props) {
    // Scalars pass through aurora unclamped, so only locks and readiness can change the label
    auto state = std::make_shared<RowState>(RowState{.gate = props.gate});
    graphics_tuner_control(ctx.window, ctx.left, ctx.right,
        GraphicsTunerProps{
            .option = props.option,
            .title = info.label,
            .helpText = props.help,
            .isDisabled = std::move(props.isDisabled),
            .unavailableReason = [state] { return state->unavailable_reason(); },
            .valueOverride = [state] { return state->value_label(); },
        });
}

void moved_tuner_row(RowContext& ctx, const GraphicsRowInfo& info, GraphicsOption option,
    const Rml::String& help, std::function<bool()> isDisabled = {}) {
    graphics_tuner_control(ctx.window, ctx.left, ctx.right,
        GraphicsTunerProps{
            .option = option,
            .title = info.label,
            .helpText = help,
            .isDisabled = std::move(isDisabled),
        });
}

struct ActionRowProps {
    const char* help;
    Rml::String (*getValue)();
    bool (*onPressed)();
    RowGate gate;
    std::function<bool()> isDisabled;
};

void action_row(RowContext& ctx, const GraphicsRowInfo& info, ActionRowProps props) {
    auto state = std::make_shared<RowState>(RowState{.gate = props.gate});
    config_action_select(ctx.left, ctx.right,
        ActionProps{
            .key = info.label,
            .helpText = props.help,
            .getValue =
                [state, getValue = props.getValue] {
                    if (Rml::String label = state->value_label(); !label.empty()) {
                        return label;
                    }
                    return getValue != nullptr ? getValue() : Rml::String{};
                },
            .onPressed = props.onPressed,
            .isDisabled = std::move(props.isDisabled),
            .unavailableReason = [state] { return state->unavailable_reason(); },
        });
}

void preset_row(RowContext& ctx, const GraphicsRowInfo& info) {
    auto& button = ctx.left.add_select_button({
        .key = info.label,
        .getValue = [] { return Rml::String{render::preset_display_name()}; },
        .isModified = [] { return render::detect_preset() == GraphicsPreset::Custom; },
    });
    ctx.left.register_control(button, ctx.right, [](Pane& pane) {
        pane.clear();
        for (const auto& option : kPresetOptions) {
            const GraphicsPreset preset = option.value;
            pane.add_button(
                    {
                        .text = option.label,
                        .isSelected = [preset] { return render::detect_preset() == preset; },
                    })
                .on_pressed([preset] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    render::apply_preset(preset);
                    config::save();
                });
        }
        pane.add_rml("Original matches the GameCube look. Low to Ultra add enhanced rendering. "
                     "Settings this device cannot run, or that are not built yet, stay off.");
    });
}

void water_refraction_row(RowContext& ctx, const GraphicsRowInfo& info) {
    auto& var = getSettings().game.disableWaterRefraction;
    auto& button = ctx.left.add_child<BoolButton>(BoolButton::Props{
        .key = info.label,
        .getValue = [&var] { return !var.getValue(); },
        .setValue =
            [&var](bool enabled) {
                if (enabled == !var.getValue()) {
                    return;
                }
                var.setValue(!enabled);
                config::save();
            },
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
    });
    ctx.left.register_control(button, ctx.right, [](Pane& pane) {
        pane.clear();
        pane.add_rml("Distort the view through water surfaces.");
    });
}

bool shadow_resolution_unused() {
    // Only Remove stops the game drawing its shadow maps; Auto still draws them outside coverage
    return ar::feature_active(ar::Feature::GameShadowSuppression) &&
           clamped().shadows.game == ar::GameShadows::Remove;
}

bool apply_recommended_preset() {
    render::apply_preset(render::recommended_preset());
    config::save();
    return true;
}

bool reset_graphics_to_preset() {
    if (!render::differs_from_reference()) {
        return false;
    }
    render::reset_to_preset();
    config::save();
    return true;
}

bool open_renderer_window() {
    g_imguiConsole.OpenRenderDebugWindow();
    close_all_documents();
    return true;
}

using S = GraphicsSection;
using F = ar::Feature;

constexpr std::array<GraphicsRow, 73> kRows{{
    {{S::Preset, "Graphics Preset", "graphics.preset", "M2", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) { preset_row(ctx, info); }},
    {{S::Preset, "Recommended", nullptr, "M2", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            action_row(ctx, info,
                {
                    .help = "The preset suggested for this GPU and graphics backend. Press to "
                            "apply it.",
                    .getValue =
                        [] {
                            return Rml::String{render::preset_name(render::recommended_preset())};
                        },
                    .onPressed = apply_recommended_preset,
                    .gate = kAlwaysGate,
                });
        }},
    {{S::Preset, "Reset to Preset", nullptr, "M2", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            action_row(ctx, info,
                {
                    .help = "Return every graphics setting to the selected preset.",
                    .getValue =
                        [] {
                            return render::differs_from_reference() ? Rml::String{} :
                                                                      Rml::String{"Matches preset"};
                        },
                    .onPressed = reset_graphics_to_preset,
                    .gate = kAlwaysGate,
                });
        }},
    {{S::Materials, "Texture Replacements", "game.enableTextureReplacements", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            moved_tuner_row(
                ctx, info, GraphicsOption::TextureReplacements, kTextureReplacementHelpText);
        }},
    {{S::Materials, "Texture Materials", "graphics.textureMaterials", "M6", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().textureMaterials,
                {
                    .help = "Use material data such as normal, roughness and height maps from "
                            "texture packs.",
                    .gate = toggle_gate<F::TextureMaterials>(),
                    .clamped = [] { return clamped().textureMaterials; },
                });
        }},
    {{S::Materials, "Stochastic Tiling", "graphics.stochasticTiling", "M6", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().stochasticTiling,
                {
                    .help = "Break up visible repetition on tiling ground textures.",
                    .gate = toggle_gate<F::StochasticTiling>(),
                    .clamped = [] { return clamped().stochasticTiling; },
                });
        }},
    {{S::Materials, "Original .mat Maps", "graphics.legacyMatMaps", "M6", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().legacyMatMaps,
                {
                    .help = "Use the normal maps from older .mat texture packs.",
                    .gate = toggle_gate<F::LegacyMatMaps>(),
                    .clamped = [] { return clamped().legacyMatMaps; },
                });
        }},
    {{S::Materials, "PBR Lighting", "graphics.pbr", "M9", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().pbr,
                {
                    .help = "Light the scene with physically based materials and lights.",
                    .gate = toggle_gate<F::Pbr>(),
                    .clamped = [] { return clamped().pbr; },
                });
        }},
    {{S::Materials, "PBR Blend", "graphics.legacyBlend", "M9", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::PbrBlend,
                    .help = "Blend between PBR lighting and the original look.",
                    .gate = param_gate<F::Pbr>(),
                    .isDisabled = [] { return !graphics().pbr.getValue(); },
                });
        }},
    {{S::Lighting, "Exposure", "graphics.exposure", "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().exposure,
                {
                    .help = "How scene brightness is set. Match Original keeps the game's "
                            "brightness.",
                    .options = kExposureOptions,
                    .gate = param_gate<F::Pbr>(),
                    .clamped = [] { return render::from_aurora(clamped().exposure); },
                });
        }},
    {{S::Lighting, "Exposure Bias", "graphics.exposureBias", "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::ExposureBias,
                    .help = "Brighten or darken the image. Not used with Match Original.",
                    .gate = param_gate<F::Pbr>(),
                    .isDisabled =
                        [] {
                            return graphics().exposure.getValue() == GraphicsExposure::MatchLegacy;
                        },
                });
        }},
    {{S::Lighting, "Tonemapper", "graphics.tonemap", "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().tonemap,
                {
                    .help = "How bright colors are compressed for display.",
                    .options = kTonemapOptions,
                    .gate = param_gate<F::Pbr>(),
                    .clamped = [] { return render::from_aurora(clamped().tonemap); },
                });
        }},
    {{S::Lighting, "Light Tuning", nullptr, "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            action_row(ctx, info,
                {
                    .help = "Adjust the strength of each kind of light.",
                    .onPressed = [] { return false; },
                    .gate = param_gate<F::Pbr>(),
                    .isDisabled = advanced_off,
                });
        }},
    {{S::Shadows, "Shadow Quality", "graphics.shadowQuality", "M13, M14", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().shadowQuality,
                {
                    .help = "Real-time shadows from the sun, the moon and other lights.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::Shadows>(),
                    .clamped = [] { return render::from_aurora(clamped().shadows.quality); },
                });
        }},
    {{S::Shadows, "Game Shadows", "graphics.gameShadows", "M13", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().gameShadows,
                {
                    .help = "What happens to the game's own shadows. Replace When Covered removes "
                            "them only where real-time shadows exist.",
                    .options = kGameShadowOptions,
                    .gate = toggle_gate<F::GameShadowSuppression>(),
                    .clamped = [] { return render::from_aurora(clamped().shadows.game); },
                    .option = render::game_shadows_option,
                });
        }},
    {{S::Shadows, "Keep Shadow Cues", "graphics.keepShadowCues", "M13", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().keepShadowCues,
                {
                    .help = "Keep the game shadows that mark gameplay targets, such as landing "
                            "spots.",
                    .gate = param_gate<F::GameShadowSuppression>(),
                    .clamped = [] { return clamped().shadows.keepCues; },
                    .isDisabled =
                        [] {
                            return graphics().gameShadows.getValue() != GraphicsGameShadows::Auto;
                        },
                });
        }},
    {{S::Shadows, "Original Shadow Strength", "graphics.legacyShadowGain", "M13", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::LegacyShadowGain,
                    .help = "Darkness of the game's own shadows.",
                    .gate = param_gate<F::GameShadowSuppression>(),
                });
        }},
    {{S::Shadows, "Original Shadow Resolution", "game.shadowResolutionMultiplier", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            moved_tuner_row(ctx, info, GraphicsOption::ShadowResolution, kShadowResolutionHelpText,
                shadow_resolution_unused);
        }},
    {{S::AoGi, "Ambient Occlusion", "graphics.ao", "M8", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().ao,
                {
                    .help = "Darken creases and contact areas.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::Ao>(),
                    .clamped = [] { return render::from_aurora(clamped().ao); },
                });
        }},
    {{S::AoGi, "Indirect Light", "graphics.ssil", "M11", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().ssil,
                {
                    .help = "Light bounced from nearby surfaces. Needs Ambient Occlusion.",
                    .gate = toggle_gate<F::Ssil>(),
                    .clamped = [] { return clamped().ssil; },
                    .isDisabled = [] { return graphics().ao.getValue() == GraphicsQuality::Off; },
                });
        }},
    {{S::AoGi, "Global Illumination", "graphics.gi", "M21, M24", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().gi,
                {
                    .help = "Light bounced across the whole scene.",
                    .options = kGiOptions,
                    .gate = toggle_gate<F::Gi>(),
                    .clamped = [] { return render::from_aurora(clamped().gi); },
                });
        }},
    {{S::Reflections, "Reflections", "graphics.reflections", "M23", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().reflections,
                {
                    .help = "Reflections on shiny and wet surfaces. Needs PBR Lighting.",
                    .options = kReflectionOptions,
                    .gate = toggle_gate<F::Reflections>(),
                    .clamped = [] { return render::from_aurora(clamped().reflections); },
                    .isDisabled =
                        [] {
                            return render::feature_gate<F::Reflections>(0).reason ==
                                   ar::Reason::NeedsPbr;
                        },
                });
        }},
    {{S::Reflections, "Water Refraction", "game.disableWaterRefraction", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) { water_refraction_row(ctx, info); }},
    {{S::RayTracing, "RT Mode", "graphics.rt", "M22, M24", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().rt,
                {
                    .help = "Ray-traced shadows, ambient occlusion and reflections. Path-Traced "
                            "Reference is a slow quality reference.",
                    .options = kRtModeOptions,
                    .gate = toggle_gate<F::Rt>(),
                    .clamped = [] { return render::from_aurora(clamped().rt); },
                });
        }},
    {{S::RayTracing, "RT Backend", "graphics.rtBackend", "M22, M25", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().rtBackend,
                {
                    .help = "Where rays are traced. Native (DXR) needs Direct3D 12.",
                    .options = kRtBackendOptions,
                    .gate = {&render::rt_backend_gate, nullptr},
                    .clamped = [] { return render::from_aurora(clamped().rtBackend); },
                });
        }},
    {{S::RayTracing, "Re-enable Native Tracing", nullptr, "M25", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            action_row(ctx, info,
                {
                    .help = "Native ray tracing was turned off after a GPU crash. Turn it back on.",
                    .getValue =
                        [] {
                            const auto support = ar::feature_support(F::NativeTrace);
                            return support.reason == ar::Reason::NativeDisabledAfterCrash ?
                                       Rml::String{render::reason_label(support.reason)} :
                                       Rml::String{};
                        },
                    // aurora has no call to re-enable native tracing yet
                    .onPressed = [] { return false; },
                    .gate = {&render::native_reenable_gate, nullptr},
                });
        }},
    {{S::Atmosphere, "Volumetric Fog", "graphics.fogVolumetric", "M16", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().fogVolumetric,
                {
                    .help = "Fog lit by the sun and other lights.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::VolumetricFog>(),
                    .clamped = [] { return render::from_aurora(clamped().fog.volumetric); },
                });
        }},
    {{S::Atmosphere, "God Rays", "graphics.godRays", "M16", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().godRays,
                {
                    .help = "Light shafts through fog, trees and clouds.",
                    .options = kGodRayOptions,
                    .gate = toggle_gate<F::GodRays>(),
                    .clamped = [] { return render::from_aurora(clamped().fog.godRays); },
                });
        }},
    {{S::Atmosphere, "Fog Scattering", "graphics.fogScattering", "M16", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::FogScattering,
                    .help = "How strongly fog scatters light.",
                    .gate = param_gate<F::VolumetricFog>(),
                });
        }},
    {{S::Atmosphere, "Shaft Strength", "graphics.shaftStrength", "M16", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::ShaftStrength,
                    .help = "Brightness of light shafts.",
                    .gate = param_gate<F::GodRays>(),
                    .isDisabled =
                        [] { return graphics().godRays.getValue() == GraphicsGodRays::Off; },
                });
        }},
    {{S::Atmosphere, "Match Original Fog", "graphics.fogLegacyMatch", "M16", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().fogLegacyMatch,
                {
                    .help = "Keep fog colors close to the original game.",
                    .gate = param_gate<F::VolumetricFog>(),
                    .clamped = [] { return clamped().fog.legacyMatch; },
                });
        }},
    {{S::Atmosphere, "Fog on Translucents", "graphics.fogTranslucents", "M16", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().fogTranslucents,
                {
                    .help = "Apply volumetric fog to water, particles and other see-through "
                            "surfaces.",
                    .gate = param_gate<F::VolumetricFog>(),
                    .clamped = [] { return clamped().fog.translucents; },
                });
        }},
    {{S::Sky, "Clouds", "graphics.clouds", "M17", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().clouds,
                {
                    .help = "Volumetric clouds that replace the game's cloud layers.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::Clouds>(),
                    .clamped = [] { return render::from_aurora(clamped().sky.clouds); },
                });
        }},
    {{S::Sky, "Cloud Style", "graphics.cloudStyle", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudStyle,
                    .help = "Blend between the game's cloud colors and physically lit clouds. "
                            "Auto picks per area.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Cloud Shadows", "graphics.cloudShadows", "M15", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().cloudShadows,
                {
                    .help = "Shadows of moving clouds on the ground. Always on with real-time sun "
                            "shadows.",
                    .gate = toggle_gate<F::CloudShadows>(),
                    .clamped = [] { return clamped().sky.cloudShadows; },
                });
        }},
    {{S::Sky, "Cloud Shadow Strength", "graphics.cloudShadowStrength", "M15", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudShadowStrength,
                    .help = "Darkness of cloud shadows.",
                    .gate = param_gate<F::CloudShadows>(),
                });
        }},
    {{S::Sky, "Keep Cloud Dome", "graphics.keepCloudDome", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().keepCloudDome,
                {
                    .help = "Keep the game's painted cloud dome behind the volumetric clouds.",
                    .gate = param_gate<F::Clouds>(),
                    .clamped = [] { return clamped().sky.keepCloudDome; },
                });
        }},
    {{S::Sky, "Cloud Coverage", "graphics.cloudCoverage", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudCoverage,
                    .help = "Amount of sky covered by clouds.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Cloud Density", "graphics.cloudDensity", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudDensity,
                    .help = "Thickness of the clouds.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Cloud Altitude", "graphics.cloudAltitude", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudAltitude,
                    .help = "Height of the cloud layer.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Cloud Wind Speed", "graphics.cloudWindSpeed", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudWindSpeed,
                    .help = "How fast the clouds move.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Silver Lining", "graphics.cloudSilverLining", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudSilverLining,
                    .help = "Brightness of cloud edges facing the sun.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Sky, "Powder", "graphics.cloudPowder", "M17", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::CloudPowder,
                    .help = "Darkening inside thick clouds.",
                    .gate = param_gate<F::Clouds>(),
                });
        }},
    {{S::Grass, "Grass Quality", "graphics.grassQuality", "M18", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().grassQuality,
                {
                    .help = "Dense, wind-driven grass drawn on the GPU.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::Grass>(),
                    .clamped = [] { return render::from_aurora(clamped().grass.quality); },
                });
        }},
    {{S::Grass, "Replace Cuttable Grass", "graphics.grassReplaceGame", "M18", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().grassReplaceGame,
                {
                    .help = "Draw the game's cuttable grass with the new grass. Cutting works as "
                            "before.",
                    .gate = param_gate<F::Grass>(),
                    .clamped = [] { return clamped().grass.replaceGamePatches; },
                });
        }},
    {{S::Grass, "Ground Grass", "graphics.grassTerrain", "M19", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().grassTerrain,
                {
                    .help = "Add short grass to grassy ground.",
                    .gate = toggle_gate<F::GrassTerrain>(),
                    .clamped = [] { return clamped().grass.terrainGrass; },
                });
        }},
    {{S::Grass, "Draw Distance", "graphics.grassDistanceM", "M18", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::GrassDistance,
                    .help = "How far grass is drawn. Auto follows Grass Quality.",
                    .gate = param_gate<F::Grass>(),
                });
        }},
    {{S::Grass, "Density", "graphics.grassDensity", "M18", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::GrassDensity,
                    .help = "Number of grass blades.",
                    .gate = param_gate<F::Grass>(),
                });
        }},
    {{S::Grass, "Distant Density", "graphics.grassDistantDensity", "M19", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::GrassDistantDensity,
                    .help = "Grass density far from the camera. Auto follows Grass Quality.",
                    .gate = param_gate<F::Grass>(),
                });
        }},
    {{S::Grass, "Grass Shadows", "graphics.grassShadows", "M19", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().grassShadows,
                {
                    .help = "Shadows cast by and onto grass blades.",
                    .options = kGrassShadowOptions,
                    .gate = param_gate<F::Grass>(),
                    .clamped = [] { return render::from_aurora(clamped().grass.shadows); },
                });
        }},
    {{S::Grass, "Original Shadows on Grass", "graphics.grassLegacyShadows", "M19", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().grassLegacyShadows,
                {
                    .help = "Show the game's own shadows on grass blades.",
                    .gate = param_gate<F::Grass>(),
                    .clamped = [] { return clamped().grass.legacyShadowTransfer; },
                });
        }},
    {{S::Grass, "Interaction", "graphics.grassInteraction", "M18", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().grassInteraction,
                {
                    .help = "Grass bends around Link, enemies and items.",
                    .gate = param_gate<F::Grass>(),
                    .clamped = [] { return clamped().grass.interaction; },
                });
        }},
    {{S::Grass, "Wind Strength", "graphics.grassWind", "M18", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::GrassWind,
                    .help = "How strongly wind moves the grass.",
                    .gate = param_gate<F::Grass>(),
                });
        }},
    {{S::Terrain, "Terrain Detail", "graphics.terrainDetail", "M20", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().terrainDetail,
                {
                    .help = "Add surface depth to ground textures. Parallax is cheaper; "
                            "Tessellated adds real geometry.",
                    .options = kTerrainDetailOptions,
                    .gate = {&render::terrain_detail_gate, &render::terrain_ready_gate},
                    .clamped = [] { return render::from_aurora(clamped().terrain.mode); },
                });
        }},
    {{S::Terrain, "Tessellation Quality", "graphics.terrainQuality", "M20", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().terrainQuality,
                {
                    .help = "Triangle size for tessellated terrain. Smaller is sharper and "
                            "slower.",
                    .options = kTessellationOptions,
                    .gate = param_gate<F::TerrainTessellation>(),
                    .clamped = [] { return render::from_aurora(clamped().terrain.quality); },
                    .isDisabled =
                        [] {
                            return graphics().terrainDetail.getValue() !=
                                   GraphicsTerrainDetail::Tessellated;
                        },
                });
        }},
    {{S::Terrain, "Displacement Strength", "graphics.terrainStrength", "M20", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::TerrainStrength,
                    .help = "Depth of terrain detail.",
                    .gate = {&render::terrain_detail_gate, nullptr},
                });
        }},
    {{S::Terrain, "Detail Distance", "graphics.terrainDistanceM", "M20", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::TerrainDistance,
                    .help = "How far terrain detail is drawn. Auto follows the quality.",
                    .gate = {&render::terrain_detail_gate, nullptr},
                });
        }},
    {{S::Terrain, "Flatten Under Feet", "graphics.terrainFlatten", "M20", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().terrainFlatten,
                {
                    .help = "Keep the ground flat under characters and objects.",
                    .gate = param_gate<F::TerrainTessellation>(),
                    .clamped = [] { return clamped().terrain.flattenVolumes; },
                });
        }},
    {{S::AntiAliasing, "TAA", "graphics.taa", "M7", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().taa,
                {
                    .help = "Temporal anti-aliasing smooths jagged edges and shimmering.",
                    .gate = toggle_gate<F::Taa>(),
                    .clamped = [] { return clamped().taa; },
                });
        }},
    {{S::AntiAliasing, "TAA Sharpness", "graphics.taaSharpness", "M7", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::TaaSharpness,
                    .help = "Sharpening applied after TAA.",
                    .gate = param_gate<F::Taa>(),
                    .isDisabled = [] { return !graphics().taa.getValue(); },
                });
        }},
    {{S::PostProcessing, "Bloom", "game.bloomMode", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            moved_tuner_row(ctx, info, GraphicsOption::BloomMode, kBloomHelpText);
        }},
    {{S::PostProcessing, "Bloom Brightness", "game.bloomMultiplier", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            moved_tuner_row(ctx, info, GraphicsOption::BloomMultiplier, kBloomBrightnessHelpText);
        }},
    {{S::PostProcessing, "Depth of Field", "game.depthOfFieldMode", "moved", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            moved_tuner_row(ctx, info, GraphicsOption::DepthOfFieldMode, kDepthOfFieldHelpText);
        }},
    {{S::Performance, "GPU Foliage", "graphics.gpuFoliage", "M26", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().gpuFoliage,
                {
                    .help = "Animate flowers and plants on the GPU.",
                    .gate = toggle_gate<F::GpuFoliage>(),
                    .clamped = [] { return clamped().gpuFoliage; },
                });
        }},
    {{S::Performance, "GPU Weather", "graphics.gpuWeather", "M26", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().gpuWeather,
                {
                    .help = "Run rain, snow and other weather effects on the GPU.",
                    .gate = toggle_gate<F::GpuWeather>(),
                    .clamped = [] { return clamped().gpuWeather; },
                });
        }},
    {{S::Performance, "GPU Particles", "graphics.gpuParticles", "M26", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().gpuParticles,
                {
                    .help = "Run particle effects on the GPU.",
                    .gate = toggle_gate<F::GpuParticles>(),
                    .clamped = [] { return clamped().gpuParticles; },
                });
        }},
    {{S::Performance, "GPU Deformation", "graphics.gpuDeform", "M26", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().gpuDeform,
                {
                    .help = "Run cutscene skin and cloth deformation on the GPU.",
                    .gate = toggle_gate<F::GpuDeform>(),
                    .clamped = [] { return clamped().gpuDeform; },
                });
        }},
    {{S::Performance, "Mesh Replacement", "graphics.meshReplacement", "M27", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().meshReplacement,
                {
                    .help = "Use replacement models from installed packs. Applies on the next "
                            "area load.",
                    .gate = toggle_gate<F::MeshReplacement>(),
                    .clamped = [] { return clamped().meshReplacement; },
                });
        }},
    {{S::Performance, "Static World GPU Replay", "graphics.staticWorld", "M28", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().staticWorld,
                {
                    .help = "Experimental: replay static scenery from the GPU. Applies on the "
                            "next area load.",
                    .options = kStaticWorldOptions,
                    .gate = toggle_gate<F::StaticWorld>(),
                    .clamped = [] { return render::from_aurora(clamped().staticWorld); },
                });
        }},
    {{S::Advanced, "Debug View", "graphics.debugView", "M3-W4", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::DebugView,
                    .help = "Show a renderer debug view instead of the final image.",
                    .gate = kNotBuiltGate,
                    .isDisabled = advanced_off,
                });
        }},
    {{S::Advanced, "Shadow Page Pool", "graphics.shadowPages", "M13", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::ShadowPagePool,
                    .help = "Memory pool for shadow map pages.",
                    .gate = param_gate<F::Shadows>(),
                    .isDisabled = advanced_off,
                });
        }},
    {{S::Advanced, "Shadow Page Budget", "graphics.shadowPageBudget", "M14", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::ShadowPageBudget,
                    .help = "Shadow map pages updated per frame.",
                    .gate = param_gate<F::Shadows>(),
                    .isDisabled = advanced_off,
                });
        }},
    {{S::Advanced, "Open Renderer Debug Window", nullptr, "M2", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            action_row(ctx, info,
                {
                    .help = "Open the renderer debug window. This closes the menu.",
                    .onPressed = open_renderer_window,
                    .gate = kAlwaysGate,
                    // Closing every document from prelaunch would skip what Play sets up
                    .isDisabled =
                        [prelaunch = ctx.prelaunch] {
                            return prelaunch || is_prelaunch_open() || advanced_off() ||
                                   !IsGameLaunched || speedrun::isActive();
                        },
                });
        }},
}};

constexpr auto kRowInfos = [] {
    std::array<GraphicsRowInfo, kRows.size()> infos{};
    for (size_t i = 0; i < kRows.size(); ++i) {
        infos[i] = kRows[i].info;
    }
    return infos;
}();

void append_item(std::string& list, std::string_view item) {
    if (item.empty()) {
        return;
    }
    if (!list.empty()) {
        list += ", ";
    }
    list += item;
}

bool is_graphics_key(const char* key) {
    return key != nullptr && std::string_view{key}.starts_with("graphics.");
}

std::string test_rows_count() {
    std::string problems;
    if (kRowInfos.size() != 73) {
        append_item(problems, fmt::format("{} rows, expected 73", kRowInfos.size()));
    }
    std::array<size_t, kSectionTitles.size()> counts{};
    size_t previous = 0;
    for (const auto& row : kRowInfos) {
        const auto section = static_cast<size_t>(row.section);
        if (section >= counts.size()) {
            append_item(problems, fmt::format("{} has an unknown section", row.label));
            continue;
        }
        if (section < previous) {
            append_item(problems, fmt::format("{} is out of section order", row.label));
        }
        previous = section;
        ++counts[section];
    }
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != kSectionRowCounts[i]) {
            append_item(problems, fmt::format("{} has {} rows, expected {}", kSectionTitles[i],
                                      counts[i], kSectionRowCounts[i]));
        }
    }
    return problems;
}

std::string test_rows_cover_every_key() {
    std::vector<std::string_view> expected{"graphics.preset"};
#define DUSK_EXPECTED_KEY(member, field) expected.emplace_back("graphics." #member);
    DUSK_GRAPHICS_FIELDS(DUSK_EXPECTED_KEY)
#undef DUSK_EXPECTED_KEY
    std::string problems;
    if (expected.size() != 62) {
        append_item(problems, fmt::format("{} graphics keys, expected 62", expected.size()));
    }
    for (const auto key : expected) {
        const auto count = std::ranges::count_if(kRowInfos,
            [key](const GraphicsRowInfo& row) { return row.key != nullptr && row.key == key; });
        if (count != 1) {
            append_item(problems, fmt::format("{} has {} rows", key, count));
        }
    }
    for (const auto& row : kRowInfos) {
        if (is_graphics_key(row.key) &&
            std::ranges::find(expected, std::string_view{row.key}) == expected.end())
        {
            append_item(problems, fmt::format("{} is not a graphics setting", row.key));
        }
    }
    return problems;
}

std::string test_rows_moved() {
    std::string problems;
    for (const auto key : kMovedKeys) {
        const bool present = std::ranges::any_of(kRowInfos,
            [key](const GraphicsRowInfo& row) { return row.key != nullptr && row.key == key; });
        if (!present) {
            append_item(problems, fmt::format("{} has no row", key));
        } else if (config::GetConfigVar(key) == nullptr) {
            append_item(problems, fmt::format("{} is not registered", key));
        }
    }
    return problems;
}

std::string test_rows_labels_unique() {
    std::string problems;
    for (size_t i = 0; i < kRowInfos.size(); ++i) {
        for (size_t j = i + 1; j < kRowInfos.size(); ++j) {
            if (std::string_view{kRowInfos[i].label} == kRowInfos[j].label) {
                append_item(problems, fmt::format("{} is used twice", kRowInfos[i].label));
            }
        }
    }
    return problems;
}

std::string test_rows_preset_flags() {
    std::string problems;
    size_t flagged = 0;
    for (const auto& row : kRowInfos) {
        if (row.presetControlled) {
            ++flagged;
        }
        // The preset row sets the preset rather than following it
        if (!is_graphics_key(row.key) || std::string_view{row.key} == "graphics.preset") {
            if (row.presetControlled) {
                append_item(
                    problems, fmt::format("{} is flagged but has no preset value", row.label));
            }
            continue;
        }
        if (row.presetControlled != render::is_preset_controlled(row.key)) {
            append_item(problems, fmt::format("{} flag does not match {}", row.label, row.key));
        }
    }
    if (flagged != 26) {
        append_item(problems, fmt::format("{} rows are preset-controlled, expected 26", flagged));
    }
    return problems;
}

}  // namespace

void build_graphics_tab(Window& window, Pane& leftPane, Pane& rightPane, bool prelaunch) {
    RowContext ctx{window, leftPane, rightPane, prelaunch};
    std::optional<GraphicsSection> section;
    for (const auto& row : kRows) {
        if (section != row.info.section) {
            section = row.info.section;
            leftPane.add_section(kSectionTitles[static_cast<size_t>(row.info.section)]);
        }
        row.build(ctx, row.info);
    }
}

std::span<const GraphicsRowInfo> graphics_rows() noexcept {
    return kRowInfos;
}

void graphics_tab_self_test(render::SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("rows.count", test_rows_count());
    check("rows.cover_every_key", test_rows_cover_every_key());
    check("rows.moved", test_rows_moved());
    check("rows.labels_unique", test_rows_labels_unique());
    check("rows.preset_flags", test_rows_preset_flags());
}

}  // namespace dusk::ui
