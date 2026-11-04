#include "settings_graphics.hpp"

#include "bool_button.hpp"
#include "graphics_tuner.hpp"
#include "modal.hpp"
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
#include <bit>
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
const Rml::String kTextureReplacementHelpText =
    "Enable installed texture replacements and the material data that comes with them.";

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
    3, 7, 4, 5, 5, 2, 3, 6, 11, 10, 5, 4, 3, 6, 4};

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

constexpr std::array<EnumOption<GraphicsUpscaler>, 5> kUpscalerOptions{{
    {GraphicsUpscaler::Off, "Off"},
    {GraphicsUpscaler::Auto, "Auto"},
    {GraphicsUpscaler::Fsr, "FSR"},
    {GraphicsUpscaler::Dlss, "DLSS"},
    {GraphicsUpscaler::Xess, "XeSS-SR"},
}};

constexpr std::array<EnumOption<GraphicsUpscalerQuality>, 5> kUpscalerQualityOptions{{
    {GraphicsUpscalerQuality::NativeAa, "Native AA"},
    {GraphicsUpscalerQuality::Quality, "Quality"},
    {GraphicsUpscalerQuality::Balanced, "Balanced"},
    {GraphicsUpscalerQuality::Performance, "Performance"},
    {GraphicsUpscalerQuality::UltraPerformance, "Ultra Performance"},
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

// One rule for every row that needs AO, so the self-test pins what the rows do
bool ao_dependent_row_disabled(GraphicsQuality ao) noexcept {
    return ao == GraphicsQuality::Off;
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

    // A note only fills the value while the row has nothing of its own to say
    Rml::String value_label(Rml::String (*note)() = nullptr) {
        const RowStatus& s = status();
        if (!s.valueLabel.empty() || note == nullptr) {
            return s.valueLabel;
        }
        return note();
    }

    Rml::String help_suffix(Rml::String (*note)() = nullptr) {
        const RowStatus& s = status();
        Rml::String suffix = s.locked ? Rml::String{} : s.reason;
        const Rml::String extra = note != nullptr ? note() : Rml::String{};
        if (extra.empty()) {
            return suffix;
        }
        return suffix.empty() ? extra : suffix + "<br/><br/>" + extra;
    }
};

struct BoolRowProps {
    const char* help;
    RowGate gate;
    bool (*clamped)();
    std::function<bool()> isDisabled;
    Rml::String (*valueNote)() = nullptr;
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
            .valueOverride = [state, note = props.valueNote] { return state->value_label(note); },
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
    Rml::String (*label)(E) = nullptr;
    void (*onChange)(E) = nullptr;
    Rml::String (*valueNote)() = nullptr;
    Rml::String (*helpNote)() = nullptr;
};

template <typename E>
void enum_row(RowContext& ctx, const GraphicsRowInfo& info, ConfigVar<E>& var,
    EnumRowProps<std::type_identity_t<E>> props) {
    const auto options = props.options;
    const auto optionOf = props.option;
    const auto labelOf = props.label;
    const RowGate gate = props.gate;
    const auto text = [options, labelOf](E value) {
        return labelOf != nullptr ? labelOf(value) : option_label(options, value);
    };
    auto state = std::make_shared<RowState>(RowState{
        .gate = gate,
        .option = [&var, optionOf] { return optionOf(var.getValue()); },
        .requested = [&var, text] { return text(var.getValue()); },
        .clamped = [text, clampedValue = props.clamped] { return text(clampedValue()); },
        .active = [&var, optionOf] { return optionOf(var.getValue()) != 0; },
    });
    config_enum_select(ctx.left, ctx.right, var,
        ConfigEnumProps<E>{
            .key = info.label,
            .helpText = props.help,
            .options = std::vector<EnumOption<E>>(options.begin(), options.end()),
            .onChange = props.onChange,
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
            .valueOverride = [state, note = props.valueNote] { return state->value_label(note); },
            .helpSuffix = [state, note = props.helpNote] { return state->help_suffix(note); },
            .optionLabel = labelOf,
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

struct LightTuningEntry {
    GraphicsOption option;
    const char* label;
    const char* help;
};

constexpr std::array<LightTuningEntry, 7> kLightTuningEntries{{
    {GraphicsOption::LightSunMoon, "Sun & Moon", "Sunlight by day and moonlight by night."},
    {GraphicsOption::LightKeyRoom, "Interior Key Light",
        "The main light indoors, where there is no sun or moon."},
    {GraphicsOption::LightRoom, "Room Lights", "The fixed lights a level places, such as lamps."},
    {GraphicsOption::LightPoint, "Torches & Point Lights",
        "Torches, fires and other small glowing objects."},
    {GraphicsOption::LightLantern, "Lantern", "Link's lantern and other lights that follow him."},
    {GraphicsOption::LightEffect, "Effect & Event Lights",
        "Brief lights from effects and lightning, and the spotlights of some boss fights."},
    {GraphicsOption::LightFill, "Camera Fill",
        "The soft light the game adds from the camera onto Link and other characters. 0% turns "
        "it off."},
}};

const Rml::String kLightTuningHelpText =
    "Adjust the strength of each kind of light. Used by PBR Lighting. Turning a light up makes it "
    "brighter but it reaches no further. With Exposure on Match Original the overall brightness "
    "still follows the original game, so these mostly change the balance between lights.";

bool light_tuning_custom() {
    bool custom = false;
#define DUSK_LIGHT_CUSTOM(member)                                                                  \
    custom = custom || graphics().member.getValue() != graphics().member.getDefaultValue();
    DUSK_LIGHT_TUNING_FIELDS(DUSK_LIGHT_CUSTOM)
#undef DUSK_LIGHT_CUSTOM
    return custom;
}

void reset_light_tuning() {
#define DUSK_LIGHT_RESET(member) graphics().member.setValue(graphics().member.getDefaultValue());
    DUSK_LIGHT_TUNING_FIELDS(DUSK_LIGHT_RESET)
#undef DUSK_LIGHT_RESET
}

void light_tuning_row(RowContext& ctx, const GraphicsRowInfo& info) {
    auto state = std::make_shared<RowState>(RowState{.gate = param_gate<ar::Feature::Pbr>()});
    std::function<bool()> locked = [state] { return !state->unavailable_reason().empty(); };
    auto& button = ctx.left.add_select_button({
        .key = info.label,
        .getValue =
            [state] {
                if (Rml::String label = state->value_label(); !label.empty()) {
                    return label;
                }
                return Rml::String{light_tuning_custom() ? "Custom" : "Default"};
            },
        .isDisabled = detail::unless_locked(locked, advanced_off),
        .isModified = light_tuning_custom,
        .isLocked = locked,
    });
    ctx.left.register_control(button, ctx.right, [state, window = &ctx.window](Pane& pane) {
        pane.clear();
        if (const Rml::String reason = state->unavailable_reason(); !reason.empty()) {
            pane.add_rml(compose_help(kLightTuningHelpText, reason));
            return;
        }
        pane.add_rml(kLightTuningHelpText);
        for (const LightTuningEntry& entry : kLightTuningEntries) {
            const GraphicsSetting& setting = GraphicsSetting::of(entry.option);
            pane.add_select_button({
                                       .key = entry.label,
                                       .getValue = [&setting] { return setting.text(); },
                                       .isModified = [&setting] { return setting.isModified(); },
                                   })
                .on_pressed([window, &entry] {
                    window->push(std::make_unique<GraphicsTuner>(GraphicsTunerProps{
                        .option = entry.option,
                        .title = entry.label,
                        .helpText = entry.help,
                    }));
                });
        }
        pane.add_button({
                            .text = "Reset Light Tuning",
                            .isDisabled = [] { return !light_tuning_custom(); },
                        })
            .on_pressed([] {
                mDoAud_seStartMenu(kSoundItemChange);
                reset_light_tuning();
                config::save();
            });
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

bool upscaler_off() {
    return graphics().upscaler.getValue() == GraphicsUpscaler::Off;
}

Rml::String taa_replaced_note() {
    const ar::UpscaleStatus status = ar::upscale_status();
    if (status.active == ar::Upscaler::Off || status.displayName.empty()) {
        return {};
    }
    return fmt::format("Replaced by {}", status.displayName);
}

Rml::String xess_path_note() {
    const ar::UpscaleStatus status = ar::upscale_status();
    if (status.active != ar::Upscaler::Xess || status.path.empty()) {
        return {};
    }
    return fmt::format("XeSS-SR is running on its {} path.", status.path);
}

// The row value only has room for the sizes, so the sentence naming them goes in the help
Rml::String upscaler_sizes_note() {
    const ar::UpscaleStatus status = ar::upscale_status();
    if (upscaler_off() || status.active == ar::Upscaler::Off) {
        return {};
    }
    return fmt::format("{}\xC3\x97{} \xE2\x86\x92 {}\xC3\x97{}", status.sceneSize[0],
        status.sceneSize[1], status.outputSize[0], status.outputSize[1]);
}

Rml::String upscaler_sizes_help() {
    const ar::UpscaleStatus status = ar::upscale_status();
    if (upscaler_off() || status.active == ar::Upscaler::Off) {
        return {};
    }
    return fmt::format("The scene renders at {}\xC3\x97{} and is rebuilt at {}\xC3\x97{}.",
        status.sceneSize[0], status.sceneSize[1], status.outputSize[0], status.outputSize[1]);
}

// The vendor that is actually running names the qualities, so Auto on DLSS shows DLAA
Rml::String upscaler_quality_text(GraphicsUpscalerQuality quality) {
    return Rml::String{render::upscaler_quality_label(clamped().upscaler, quality)};
}

void offer_native_reenable(GraphicsUpscaler value) {
    const GraphicsUpscaler vendor =
        value == GraphicsUpscaler::Auto ? GraphicsUpscaler::Dlss : value;
    const ar::FeatureSupport support =
        ar::feature_support(ar::Feature::Upscaler, enum_option(vendor));
    if (support.reason != ar::Reason::NativeDisabledAfterCrash) {
        return;
    }
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Upscaler Disabled",
        .bodyText = Rml::String{render::upscaler_reason_help(enum_option(vendor), support.reason)},
        .actions =
            {
                ModalAction{
                    .label = "Not Now",
                    .onPressed = dismiss,
                },
                ModalAction{
                    .label = "Re-enable",
                    .onPressed =
                        [](Modal& modal) {
                            ar::reenable_native_upscalers();
                            modal.pop();
                        },
                },
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
}

using S = GraphicsSection;
using F = ar::Feature;

constexpr RowGate kUpscalerGate{&render::feature_gate<F::Upscaler>,
    &render::feature_ready_gate<F::Upscaler>, &render::upscaler_reason_help};
constexpr RowGate kUpscalerQualityGate{
    &render::upscaler_quality_gate, nullptr, &render::upscaler_reason_help};

constexpr std::array<GraphicsRow, 78> kRows{{
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
                    .help = "Apply texture material data from Dusklight and texture packs, such "
                            "as which ground textures use stochastic tiling.",
                    .gate = toggle_gate<F::TextureMaterials>(),
                    .clamped = [] { return clamped().textureMaterials; },
                });
        }},
    {{S::Materials, "Stochastic Tiling", "graphics.stochasticTiling", "M6", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().stochasticTiling,
                {
                    .help = "Break up visible repetition on tiling ground textures. Needs "
                            "Texture Materials.",
                    .gate = toggle_gate<F::StochasticTiling>(),
                    .clamped =
                        [] { return clamped().textureMaterials && clamped().stochasticTiling; },
                    .isDisabled = [] { return !graphics().textureMaterials.getValue(); },
                });
        }},
    {{S::Materials, "Original .mat Maps", "graphics.legacyMatMaps", "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().legacyMatMaps,
                {
                    .help = "Use the normal maps from older .mat texture packs. Needs Texture "
                            "Materials and PBR Lighting.",
                    .gate = toggle_gate<F::LegacyMatMaps>(),
                    .clamped = [] { return clamped().textureMaterials && clamped().legacyMatMaps; },
                    .isDisabled =
                        [] { return !graphics().textureMaterials.getValue() || !clamped().pbr; },
                });
        }},
    {{S::Materials, "Generate Missing Mipmaps", "graphics.generateMissingMips", "M6", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().generateMissingMips,
                {
                    .help = "Create the smaller texture levels that original textures and some "
                            "texture-pack textures leave out, so distant surfaces shimmer less. "
                            "Menus and other 2D art keep their original look.",
                    .gate = kAlwaysGate,
                    .clamped = [] { return clamped().generateMissingMips; },
                });
        }},
    {{S::Materials, "PBR Lighting", "graphics.pbr", "M10", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().pbr,
                {
                    .help = "Light the scene with physically based materials and lights. Uses "
                            "more GPU time and video memory, more so at high Internal Resolution. "
                            "Until real-time shadows arrive, many of the game's own shadows are "
                            "not drawn on PBR-lit surfaces.",
                    .gate = toggle_gate<F::Pbr>(),
                    .clamped = [] { return clamped().pbr; },
                });
        }},
    {{S::Materials, "PBR Blend", "graphics.legacyBlend", "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::PbrBlend,
                    .help = "How much of the PBR lighting to show. 100% is full PBR; 0% shows the "
                            "original look but keeps most of the cost.",
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
                    .isDisabled = [] { return !clamped().pbr; },
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
                            return graphics().exposure.getValue() ==
                                       GraphicsExposure::MatchLegacy ||
                                   !clamped().pbr;
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
                    .isDisabled = [] { return !clamped().pbr; },
                });
        }},
    {{S::Lighting, "Light Tuning", nullptr, "M10", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) { light_tuning_row(ctx, info); }},
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
                    .help = "Darken creases, corners and the ground where objects touch it, "
                            "without dimming distant fog. Its cost grows with Internal "
                            "Resolution. Ultra is sharper but costs much more.",
                    .options = kQualityOptions,
                    .gate = toggle_gate<F::Ao>(),
                    .clamped = [] { return render::from_aurora(clamped().ao); },
                });
        }},
    {{S::AoGi, "AO Radius", "graphics.aoRadius", "M8", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::AoRadius,
                    .help = "How far the darkening spreads from creases and contact areas, "
                            "and how far bounced light reaches.",
                    .gate = param_gate<F::Ao>(),
                    .isDisabled =
                        [] { return ao_dependent_row_disabled(graphics().ao.getValue()); },
                });
        }},
    {{S::AoGi, "AO Strength", "graphics.aoStrength", "M8", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::AoStrength,
                    .help = "How dark creases and contact areas get; with Indirect Light it also "
                            "sets how much bounced light fills them. 0% turns both off but keeps "
                            "the cost.",
                    .gate = param_gate<F::Ao>(),
                    .isDisabled =
                        [] { return ao_dependent_row_disabled(graphics().ao.getValue()); },
                });
        }},
    {{S::AoGi, "Indirect Light", "graphics.ssil", "M11", true},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            bool_row(ctx, info, graphics().ssil,
                {
                    .help = "Light bounced from nearby surfaces into the creases "
                            "Ambient Occlusion darkens, tinted by what they face. It reaches as "
                            "far as AO Radius and follows AO Strength. Its cost grows with "
                            "Internal Resolution. Needs Ambient Occlusion.",
                    .gate = toggle_gate<F::Ssil>(),
                    .clamped = [] { return clamped().ssil; },
                    .isDisabled =
                        [] { return ao_dependent_row_disabled(graphics().ao.getValue()); },
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
                    .help = "Smooth jagged edges and shimmering by blending each frame with the "
                            "ones before it. The HUD, menus and most particles are not affected. "
                            "Its cost grows with Internal Resolution.",
                    .gate = toggle_gate<F::Taa>(),
                    .clamped = [] { return clamped().taa; },
                    .valueNote = taa_replaced_note,
                });
        }},
    {{S::AntiAliasing, "Sharpness", "graphics.taaSharpness", "M7", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            tuner_row(ctx, info,
                {
                    .option = GraphicsOption::TaaSharpness,
                    .help = "Sharpen the resolved image to offset temporal softening. Applies to "
                            "TAA and every upscaler.",
                    .gate = param_gate<F::Taa>(),
                    .isDisabled = [] { return !graphics().taa.getValue() && upscaler_off(); },
                });
        }},
    {{S::AntiAliasing, "Upscaler", "graphics.upscaler", "M7U", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().upscaler,
                {
                    .help = "Render the 3D scene at a lower resolution and rebuild it at full "
                            "resolution with temporal upscaling. The HUD, menus and the game's "
                            "post effects stay at full resolution. Replaces TAA while on.",
                    .options = kUpscalerOptions,
                    .gate = kUpscalerGate,
                    .clamped = [] { return render::from_aurora(clamped().upscaler); },
                    .onChange = offer_native_reenable,
                    .helpNote = xess_path_note,
                });
        }},
    {{S::AntiAliasing, "Upscaler Quality", "graphics.upscalerQuality", "M7U", false},
        [](RowContext& ctx, const GraphicsRowInfo& info) {
            enum_row(ctx, info, graphics().upscalerQuality,
                {
                    .help = "How far below the output resolution the scene renders. Lower quality "
                            "runs faster and looks softer.",
                    .options = kUpscalerQualityOptions,
                    .gate = kUpscalerQualityGate,
                    .clamped = [] { return render::from_aurora(clamped().upscalerQuality); },
                    .isDisabled = upscaler_off,
                    .label = upscaler_quality_text,
                    .valueNote = upscaler_sizes_note,
                    .helpNote = upscaler_sizes_help,
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
    if (kRowInfos.size() != 78) {
        append_item(problems, fmt::format("{} rows, expected 78", kRowInfos.size()));
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
    if (expected.size() != 67) {
        append_item(problems, fmt::format("{} graphics keys, expected 67", expected.size()));
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
    if (flagged != 27) {
        append_item(problems, fmt::format("{} rows are preset-controlled, expected 27", flagged));
    }
    return problems;
}

std::string test_rows_light_tuning() {
    std::string problems;
    const auto row = std::ranges::find_if(kRowInfos,
        [](const GraphicsRowInfo& info) { return std::string_view{info.label} == "Light Tuning"; });
    if (row == kRowInfos.end() || row->section != GraphicsSection::Lighting ||
        row->key != nullptr || std::string_view{row->unlockedIn} != "M10")
    {
        append_item(problems, "the Light Tuning row is missing or moved");
    }
    std::vector<std::string_view> names;
    for (const LightTuningEntry& entry : kLightTuningEntries) {
        const GraphicsSetting& setting = GraphicsSetting::of(entry.option);
        names.emplace_back(setting.cvarName());
        if (setting.min != 0 || setting.max != 200 || setting.step != 5 ||
            setting.defaultValue != 100 || setting.referenceValue() != 100)
        {
            append_item(problems, fmt::format("{} range", entry.label));
        }
        const bool fill = entry.option == GraphicsOption::LightFill;
        if (setting.format_value(0) != (fill ? "Off" : "0%") || setting.format_value(150) != "150%")
        {
            append_item(problems, fmt::format("{} labels", entry.label));
        }
    }
    std::vector<std::string_view> expected;
#define DUSK_LIGHT_KEY(member) expected.emplace_back("graphics." #member);
    DUSK_LIGHT_TUNING_FIELDS(DUSK_LIGHT_KEY)
#undef DUSK_LIGHT_KEY
    if (names != expected) {
        append_item(problems, "the entries do not follow the light keys");
    }
    return problems;
}

std::string test_rows_pbr_blend_share() {
    std::string problems;
    const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
    if (bits(PbrShareMap::from_tuner(0)) != bits(1.0f) ||
        bits(PbrShareMap::from_tuner(100)) != bits(0.0f) || PbrShareMap::to_tuner(1.0f) != 0 ||
        PbrShareMap::to_tuner(0.0f) != 100)
    {
        append_item(problems, "0% and 100% do not store 1 and 0 exactly");
    }
    for (int share = 0; share <= 100; share += 5) {
        if (PbrShareMap::to_tuner(PbrShareMap::from_tuner(share)) != share) {
            append_item(problems, fmt::format("{}% does not round trip", share));
        }
    }
    const GraphicsSetting& setting = GraphicsSetting::of(GraphicsOption::PbrBlend);
    if (setting.min != 0 || setting.max != 100 || setting.step != 5 || setting.defaultValue != 100)
    {
        append_item(problems, "PBR Blend is not 0-100% in 5% steps with 100% by default");
    }
    return problems;
}

std::string test_indirect_light_follows_ao() {
    std::string problems;
    const auto row = std::ranges::find_if(kRowInfos, [](const GraphicsRowInfo& info) {
        return info.key != nullptr && std::string_view{info.key} == "graphics.ssil";
    });
    if (row == kRowInfos.end() || std::string_view{row->label} != "Indirect Light" ||
        row->section != GraphicsSection::AoGi || !row->presetControlled)
    {
        append_item(problems, "the Indirect Light row is missing or moved");
    }
    for (const auto& option : kQualityOptions) {
        const bool off = option.value == GraphicsQuality::Off;
        if (ao_dependent_row_disabled(option.value) != off) {
            append_item(problems,
                fmt::format("AO {} {} the AO rows", option.label, off ? "keeps" : "disables"));
        }
    }
    if (render::reason_help(ar::Reason::NeedsAo) != "Needs Ambient Occlusion.") {
        append_item(problems, "NeedsAo does not read Needs Ambient Occlusion.");
    }
    ar::Settings context = render::last_requested();
    context.ao = ar::Quality::High;
    context.ssil = true;
    const ar::FeatureSupport on = ar::feature_support(F::Ssil, 1, context);
    if (on.supported) {
        context.ao = ar::Quality::Off;
        const ar::FeatureSupport off = ar::feature_support(F::Ssil, 1, context);
        if (off.supported || off.reason != ar::Reason::NeedsAo) {
            append_item(problems, fmt::format("AO Off gives {}", ar::reason_name(off.reason)));
        }
    } else if (on.reason != ar::Reason::NeedsCore && on.reason != ar::Reason::MsaaActive &&
               on.reason != ar::Reason::ColorBudget)
    {
        append_item(problems, fmt::format("AO High gives {}", ar::reason_name(on.reason)));
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
    check("rows.light_tuning", test_rows_light_tuning());
    check("rows.pbr_blend_share", test_rows_pbr_blend_share());
    check("rows.indirect_light_follows_ao", test_indirect_light_follows_ao());
}

}  // namespace dusk::ui
