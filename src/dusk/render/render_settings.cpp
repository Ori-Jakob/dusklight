#include "dusk/render/render_settings.hpp"

#include "dusk/render/graphics_fields.inl"

#include <fmt/format.h>
#include <magic_enum.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <type_traits>

namespace dusk::render {
namespace {
namespace ar = aurora::render;

#define DUSK_PRESET_FIELDS(X)                                                                      \
    X(stochasticTiling)                                                                            \
    X(generateMissingMips)                                                                         \
    X(taa)                                                                                         \
    X(ao)                                                                                          \
    X(pbr)                                                                                         \
    X(shadowQuality)                                                                               \
    X(shadowPages)                                                                                 \
    X(shadowPageBudget)                                                                            \
    X(gameShadows)                                                                                 \
    X(ssil)                                                                                        \
    X(gi)                                                                                          \
    X(reflections)                                                                                 \
    X(rt)                                                                                          \
    X(fogVolumetric)                                                                               \
    X(godRays)                                                                                     \
    X(clouds)                                                                                      \
    X(cloudShadows)                                                                                \
    X(grassQuality)                                                                                \
    X(grassDistanceM)                                                                              \
    X(grassDistantDensity)                                                                         \
    X(grassShadows)                                                                                \
    X(terrainDetail)                                                                               \
    X(terrainQuality)                                                                              \
    X(gpuFoliage)                                                                                  \
    X(gpuWeather)                                                                                  \
    X(gpuParticles)                                                                                \
    X(gpuDeform)

template <typename>
struct VarValue;

template <typename T>
struct VarValue<config::ConfigVar<T>> {
    using type = T;
};

#define DUSK_CHECK_PRESET_FIELD(member)                                                            \
    static_assert(std::is_same_v<decltype(PresetValues::member),                                   \
        VarValue<decltype(std::declval<UserSettings&>().graphics.member)>::type>);
DUSK_PRESET_FIELDS(DUSK_CHECK_PRESET_FIELD)
#undef DUSK_CHECK_PRESET_FIELD

#define DUSK_PRESET_KEY(member) std::string_view{"graphics." #member},
constexpr std::array kPresetKeys{DUSK_PRESET_FIELDS(DUSK_PRESET_KEY)};
#undef DUSK_PRESET_KEY
static_assert(kPresetKeys.size() == 27);

constexpr std::array<PresetValues, 5> kPresets{{
    {
        .stochasticTiling = false,
        .generateMissingMips = false,
        .taa = false,
        .ao = GraphicsQuality::Off,
        .pbr = false,
        .shadowQuality = GraphicsQuality::Off,
        .shadowPages = 1024,
        .shadowPageBudget = 64,
        .gameShadows = GraphicsGameShadows::Auto,
        .ssil = false,
        .gi = GraphicsGi::Off,
        .reflections = GraphicsReflections::Off,
        .rt = GraphicsRtMode::Off,
        .fogVolumetric = GraphicsQuality::Off,
        .godRays = GraphicsGodRays::Off,
        .clouds = GraphicsQuality::Off,
        .cloudShadows = false,
        .grassQuality = GraphicsQuality::Off,
        .grassDistanceM = 0,
        .grassDistantDensity = 0.0f,
        .grassShadows = GraphicsGrassShadows::Contact,
        .terrainDetail = GraphicsTerrainDetail::Off,
        .terrainQuality = GraphicsQuality::High,
        .gpuFoliage = false,
        .gpuWeather = false,
        .gpuParticles = false,
        .gpuDeform = false,
    },
    {
        .stochasticTiling = true,
        .generateMissingMips = true,
        .taa = true,
        .ao = GraphicsQuality::Low,
        .pbr = false,
        .shadowQuality = GraphicsQuality::Off,
        .shadowPages = 1024,
        .shadowPageBudget = 64,
        .gameShadows = GraphicsGameShadows::Auto,
        .ssil = false,
        .gi = GraphicsGi::Off,
        .reflections = GraphicsReflections::Off,
        .rt = GraphicsRtMode::Off,
        .fogVolumetric = GraphicsQuality::Low,
        .godRays = GraphicsGodRays::Auto,
        .clouds = GraphicsQuality::Off,
        .cloudShadows = true,
        .grassQuality = GraphicsQuality::Low,
        .grassDistanceM = 0,
        .grassDistantDensity = 0.0f,
        .grassShadows = GraphicsGrassShadows::Off,
        .terrainDetail = GraphicsTerrainDetail::Off,
        .terrainQuality = GraphicsQuality::High,
        .gpuFoliage = true,
        .gpuWeather = true,
        .gpuParticles = true,
        .gpuDeform = false,
    },
    {
        .stochasticTiling = true,
        .generateMissingMips = true,
        .taa = true,
        .ao = GraphicsQuality::Medium,
        .pbr = true,
        .shadowQuality = GraphicsQuality::Medium,
        .shadowPages = 1024,
        .shadowPageBudget = 32,
        .gameShadows = GraphicsGameShadows::Auto,
        .ssil = false,
        .gi = GraphicsGi::Off,
        .reflections = GraphicsReflections::Off,
        .rt = GraphicsRtMode::Off,
        .fogVolumetric = GraphicsQuality::Medium,
        .godRays = GraphicsGodRays::Auto,
        .clouds = GraphicsQuality::Low,
        .cloudShadows = true,
        .grassQuality = GraphicsQuality::Medium,
        .grassDistanceM = 0,
        .grassDistantDensity = 0.0f,
        .grassShadows = GraphicsGrassShadows::Contact,
        .terrainDetail = GraphicsTerrainDetail::Parallax,
        .terrainQuality = GraphicsQuality::High,
        .gpuFoliage = true,
        .gpuWeather = true,
        .gpuParticles = true,
        .gpuDeform = true,
    },
    {
        .stochasticTiling = true,
        .generateMissingMips = true,
        .taa = true,
        .ao = GraphicsQuality::High,
        .pbr = true,
        .shadowQuality = GraphicsQuality::High,
        .shadowPages = 1024,
        .shadowPageBudget = 64,
        .gameShadows = GraphicsGameShadows::Auto,
        .ssil = true,
        .gi = GraphicsGi::Voxel,
        .reflections = GraphicsReflections::Ssr,
        .rt = GraphicsRtMode::Off,
        .fogVolumetric = GraphicsQuality::High,
        .godRays = GraphicsGodRays::Auto,
        .clouds = GraphicsQuality::Low,
        .cloudShadows = true,
        .grassQuality = GraphicsQuality::Medium,
        .grassDistanceM = 0,
        .grassDistantDensity = 0.0f,
        .grassShadows = GraphicsGrassShadows::Contact,
        .terrainDetail = GraphicsTerrainDetail::Tessellated,
        .terrainQuality = GraphicsQuality::Medium,
        .gpuFoliage = true,
        .gpuWeather = true,
        .gpuParticles = true,
        .gpuDeform = true,
    },
    {
        .stochasticTiling = true,
        .generateMissingMips = true,
        .taa = true,
        .ao = GraphicsQuality::High,
        .pbr = true,
        .shadowQuality = GraphicsQuality::Ultra,
        .shadowPages = 2048,
        .shadowPageBudget = 64,
        .gameShadows = GraphicsGameShadows::Auto,
        .ssil = true,
        .gi = GraphicsGi::Ddgi,
        .reflections = GraphicsReflections::SsrRt,
        .rt = GraphicsRtMode::ShadowsAoReflections,
        .fogVolumetric = GraphicsQuality::Ultra,
        .godRays = GraphicsGodRays::Auto,
        .clouds = GraphicsQuality::High,
        .cloudShadows = true,
        .grassQuality = GraphicsQuality::Ultra,
        .grassDistanceM = 0,
        .grassDistantDensity = 0.0f,
        .grassShadows = GraphicsGrassShadows::Full,
        .terrainDetail = GraphicsTerrainDetail::Tessellated,
        .terrainQuality = GraphicsQuality::High,
        .gpuFoliage = true,
        .gpuWeather = true,
        .gpuParticles = true,
        .gpuDeform = true,
    },
}};

constexpr std::array kBasePresets{
    GraphicsPreset::Original,
    GraphicsPreset::Low,
    GraphicsPreset::Medium,
    GraphicsPreset::High,
    GraphicsPreset::Ultra,
};

// Discrete GPUs below this get Medium, matching aurora's LowVram threshold
constexpr uint64_t kHighPresetVramBytes = 6ull << 30;

constexpr bool is_base_preset(GraphicsPreset preset) noexcept {
    return preset >= GraphicsPreset::Original && preset <= GraphicsPreset::Ultra;
}

template <typename Fn>
void visit_preset_fields(Fn&& fn) {
    auto& graphics = getSettings().graphics;
#define DUSK_VISIT_PRESET_FIELD(member)                                                            \
    fn(std::string_view{"graphics." #member}, graphics.member,                                     \
        [](auto& values) -> auto& { return values.member; });
    DUSK_PRESET_FIELDS(DUSK_VISIT_PRESET_FIELD)
#undef DUSK_VISIT_PRESET_FIELD
}

GraphicsPreset detect_from(const PresetValues& values, GraphicsPreset base, bool meshReplacement,
    GraphicsStaticWorld staticWorld) noexcept {
    // Presets never turn these on, so naming a preset while either is on would be wrong
    if (meshReplacement || staticWorld != GraphicsStaticWorld::Off) {
        return GraphicsPreset::Custom;
    }
    return is_base_preset(base) && values == preset_values(base) ? base : GraphicsPreset::Custom;
}

// A config saved before the key existed would otherwise read as Custom on its own preset
std::optional<bool> seeded_generate_missing_mips(GraphicsPreset savedPreset, bool saved) noexcept {
    if (saved) {
        return std::nullopt;
    }
    return preset_values(savedPreset).generateMissingMips;
}

template <typename T>
std::string cell_text(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::string{magic_enum::enum_name(value)};
    } else {
        return fmt::format("{}", value);
    }
}

void append_item(std::string& list, std::string_view item) {
    if (item.empty()) {
        return;
    }
    if (!list.empty()) {
        list += ", ";
    }
    list += item;
}

std::string test_presets_distinct() {
    std::string problems;
    for (const auto p : kBasePresets) {
        size_t matches = 0;
        for (const auto q : kBasePresets) {
            if (preset_values(q) == preset_values(p)) {
                ++matches;
            }
        }
        if (matches != 1) {
            append_item(problems, fmt::format("{} matches {} presets", preset_name(p), matches));
        }
    }
    if (!(preset_values(GraphicsPreset::Custom) == preset_values(GraphicsPreset::Original))) {
        append_item(problems, "Custom does not return the Original row");
    }
    return problems;
}

std::string test_presets_original_is_default() {
    const PresetValues original = preset_values(GraphicsPreset::Original);
    const PresetValues defaults = read_preset_values(SettingsSource::Defaults);
    std::string problems;
    visit_preset_fields([&](std::string_view key, const auto&, auto get) {
        if (!(get(original) == get(defaults))) {
            append_item(problems, fmt::format("{} is {}, default {}", key, cell_text(get(original)),
                                      cell_text(get(defaults))));
        }
    });
    return problems;
}

std::string test_presets_high_trimmed() {
    const PresetValues& high = preset_values(GraphicsPreset::High);
    const PresetValues& ultra = preset_values(GraphicsPreset::Ultra);
    std::string problems;
    if (high.clouds != GraphicsQuality::Low || high.grassQuality != GraphicsQuality::Medium ||
        high.grassShadows != GraphicsGrassShadows::Contact)
    {
        append_item(
            problems, "High is not trimmed to Low clouds and Medium contact-shadowed grass");
    }
    if (ultra.clouds != GraphicsQuality::High || ultra.grassQuality != GraphicsQuality::Ultra ||
        ultra.grassShadows != GraphicsGrassShadows::Full)
    {
        append_item(problems, "Ultra does not have High clouds and Ultra fully shadowed grass");
    }
    return problems;
}

struct ExpectedPresetRow {
    std::string_view key;
    std::array<std::string_view, 5> values;
};

// Written per key, unlike kPresets, so a slip in either table shows up as a mismatch
constexpr std::array<ExpectedPresetRow, 27> kExpectedPresetTable{{
    {"graphics.stochasticTiling", {"false", "true", "true", "true", "true"}},
    {"graphics.generateMissingMips", {"false", "true", "true", "true", "true"}},
    {"graphics.taa", {"false", "true", "true", "true", "true"}},
    {"graphics.ao", {"Off", "Low", "Medium", "High", "High"}},
    {"graphics.pbr", {"false", "false", "true", "true", "true"}},
    {"graphics.shadowQuality", {"Off", "Off", "Medium", "High", "Ultra"}},
    {"graphics.shadowPages", {"1024", "1024", "1024", "1024", "2048"}},
    {"graphics.shadowPageBudget", {"64", "64", "32", "64", "64"}},
    {"graphics.gameShadows", {"Auto", "Auto", "Auto", "Auto", "Auto"}},
    {"graphics.ssil", {"false", "false", "false", "true", "true"}},
    {"graphics.gi", {"Off", "Off", "Off", "Voxel", "Ddgi"}},
    {"graphics.reflections", {"Off", "Off", "Off", "Ssr", "SsrRt"}},
    {"graphics.rt", {"Off", "Off", "Off", "Off", "ShadowsAoReflections"}},
    {"graphics.fogVolumetric", {"Off", "Low", "Medium", "High", "Ultra"}},
    {"graphics.godRays", {"Off", "Auto", "Auto", "Auto", "Auto"}},
    {"graphics.clouds", {"Off", "Off", "Low", "Low", "High"}},
    {"graphics.cloudShadows", {"false", "true", "true", "true", "true"}},
    {"graphics.grassQuality", {"Off", "Low", "Medium", "Medium", "Ultra"}},
    {"graphics.grassDistanceM", {"0", "0", "0", "0", "0"}},
    {"graphics.grassDistantDensity", {"0", "0", "0", "0", "0"}},
    {"graphics.grassShadows", {"Contact", "Off", "Contact", "Contact", "Full"}},
    {"graphics.terrainDetail", {"Off", "Off", "Parallax", "Tessellated", "Tessellated"}},
    {"graphics.terrainQuality", {"High", "High", "High", "Medium", "High"}},
    {"graphics.gpuFoliage", {"false", "true", "true", "true", "true"}},
    {"graphics.gpuWeather", {"false", "true", "true", "true", "true"}},
    {"graphics.gpuParticles", {"false", "true", "true", "true", "true"}},
    {"graphics.gpuDeform", {"false", "false", "true", "true", "true"}},
}};

std::string test_presets_table() {
    std::string problems;
    size_t visited = 0;
    visit_preset_fields([&](std::string_view key, const auto&, auto get) {
        const auto row = std::ranges::find(kExpectedPresetTable, key, &ExpectedPresetRow::key);
        if (row == kExpectedPresetTable.end()) {
            append_item(problems, fmt::format("{} has no expected values", key));
            return;
        }
        ++visited;
        for (size_t i = 0; i < kBasePresets.size(); ++i) {
            const std::string actual = cell_text(get(preset_values(kBasePresets[i])));
            if (actual != row->values[i]) {
                append_item(problems, fmt::format("{} {} is {}, expected {}", key,
                                          preset_name(kBasePresets[i]), actual, row->values[i]));
            }
        }
    });
    if (visited != kExpectedPresetTable.size()) {
        append_item(problems, fmt::format("{} of {} expected keys are preset fields", visited,
                                  kExpectedPresetTable.size()));
    }
    return problems;
}

std::string test_presets_recommend() {
    struct Case {
        RecommendInput input;
        GraphicsPreset expected;
    };
    constexpr uint64_t kGiB = 1ull << 30;
    const std::array cases{
        Case{{BACKEND_NULL, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 12 * kGiB},
            GraphicsPreset::Original},
        Case{{BACKEND_D3D12, false, FEATURE_PROFILE_COMPAT, ar::AdapterKind::Discrete, 12 * kGiB},
            GraphicsPreset::Original},
        Case{{BACKEND_METAL, true, FEATURE_PROFILE_METAL32, ar::AdapterKind::Discrete, 12 * kGiB},
            GraphicsPreset::Low},
        Case{{BACKEND_VULKAN, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Cpu, 0},
            GraphicsPreset::Original},
        Case{{BACKEND_VULKAN, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Integrated, 16 * kGiB},
            GraphicsPreset::Low},
        Case{{BACKEND_D3D11, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 12 * kGiB},
            GraphicsPreset::Low},
        Case{{BACKEND_D3D12, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 4 * kGiB},
            GraphicsPreset::Medium},
        Case{{BACKEND_D3D12, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 12 * kGiB},
            GraphicsPreset::High},
        Case{{BACKEND_D3D12, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Unknown, 12 * kGiB},
            GraphicsPreset::Low},
        Case{{BACKEND_VULKAN, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 6 * kGiB},
            GraphicsPreset::High},
        Case{{BACKEND_METAL, true, FEATURE_PROFILE_FULL, ar::AdapterKind::Discrete, 0},
            GraphicsPreset::High},
    };
    std::string problems;
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        const GraphicsPreset actual = recommend(c.input);
        if (actual != c.expected) {
            append_item(problems,
                fmt::format("case {} ({}, {}, {} bytes) gives {}, expected {}", i,
                    ar::adapter_kind_name(c.input.adapter), c.input.core ? "core" : "compat",
                    c.input.vramBytes, preset_name(actual), preset_name(c.expected)));
        }
    }
    return problems;
}

std::string test_presets_experimental_custom() {
    const PresetValues& high = preset_values(GraphicsPreset::High);
    std::string problems;
    for (const auto p : kBasePresets) {
        if (detect_from(preset_values(p), p, false, GraphicsStaticWorld::Off) != p) {
            append_item(problems, fmt::format("the {} values with base {} are not detected as {}",
                                      preset_name(p), preset_name(p), preset_name(p)));
        }
    }
    if (detect_from(preset_values(GraphicsPreset::Ultra), GraphicsPreset::High, false,
            GraphicsStaticWorld::Off) != GraphicsPreset::Custom)
    {
        append_item(problems, "the Ultra values with base High are not Custom");
    }
    PresetValues edited = high;
    edited.ao = GraphicsQuality::Low;
    if (detect_from(edited, GraphicsPreset::High, false, GraphicsStaticWorld::Off) !=
        GraphicsPreset::Custom)
    {
        append_item(problems, "High with Low ambient occlusion is not Custom");
    }
    if (detect_from(high, GraphicsPreset::High, true, GraphicsStaticWorld::Off) !=
        GraphicsPreset::Custom)
    {
        append_item(problems, "mesh replacement does not make High Custom");
    }
    for (const auto mode : {GraphicsStaticWorld::Validate, GraphicsStaticWorld::On}) {
        if (detect_from(high, GraphicsPreset::High, false, mode) != GraphicsPreset::Custom) {
            append_item(problems, fmt::format("static world {} does not make High Custom",
                                      magic_enum::enum_name(mode)));
        }
    }
    return problems;
}

std::string test_presets_reference() {
    const PresetValues& base = preset_values(base_preset());
    std::string problems;
    visit_preset_fields([&](std::string_view key, const auto& var, auto get) {
        if (!(reference_value(var) == get(base))) {
            append_item(problems, fmt::format("{} reference is {}, preset has {}", key,
                                      cell_text(reference_value(var)), cell_text(get(base))));
        }
    });
    const auto& graphics = getSettings().graphics;
    if (reference_value(graphics.taaSharpness) != 0.25f) {
        append_item(problems, fmt::format("graphics.taaSharpness reference is {}",
                                  reference_value(graphics.taaSharpness)));
    }
    if (reference_value(graphics.textureMaterials) != graphics.textureMaterials.getDefaultValue()) {
        append_item(problems, "graphics.textureMaterials reference is not its default");
    }
    return problems;
}

std::string test_presets_seed_missing_key() {
    const bool unset = getSettings().graphics.generateMissingMips.getDefaultValue();
    std::string problems;
    const auto expect = [&](GraphicsPreset base, PresetValues loaded, bool expected,
                            GraphicsPreset expectedPreset) {
        loaded.generateMissingMips = unset;
        const std::optional<bool> seeded = seeded_generate_missing_mips(base, false);
        if (!seeded.has_value()) {
            append_item(problems, fmt::format("a {} config is not seeded", preset_name(base)));
            return;
        }
        loaded.generateMissingMips = *seeded;
        const GraphicsPreset detected = detect_from(loaded, base, false, GraphicsStaticWorld::Off);
        if (*seeded != expected || detected != expectedPreset) {
            append_item(problems, fmt::format("a {} config loads as {} with the key {}",
                                      preset_name(base), preset_name(detected), *seeded));
        }
    };
    expect(GraphicsPreset::High, preset_values(GraphicsPreset::High), true, GraphicsPreset::High);
    expect(GraphicsPreset::Original, preset_values(GraphicsPreset::Original), false,
        GraphicsPreset::Original);
    PresetValues customHigh = preset_values(GraphicsPreset::High);
    customHigh.ao = GraphicsQuality::Low;
    expect(GraphicsPreset::High, customHigh, true, GraphicsPreset::Custom);
    if (seeded_generate_missing_mips(GraphicsPreset::High, true).has_value()) {
        append_item(problems, "a saved key is seeded again");
    }
    return problems;
}

std::string test_reasons_complete() {
    constexpr std::array kHardReasons{
        ar::Reason::NeedsCore,
        ar::Reason::ColorBudget,
        ar::Reason::NoFragmentStorage,
        ar::Reason::NoVertexStorage,
        ar::Reason::NoAtomic64,
        ar::Reason::NoMdi,
        ar::Reason::NeedsD3D12,
        ar::Reason::NativeUnavailable,
        ar::Reason::NativeDisabledAfterCrash,
        ar::Reason::MsaaActive,
        ar::Reason::NeedsRetainedScene,
        ar::Reason::NeedsCompanions,
        ar::Reason::BindingLimit,
        ar::Reason::BuildDisabled,
        ar::Reason::NotImplemented,
    };
    std::string problems;
    unsigned named = 0;
    for (unsigned raw = 0; raw < 256; ++raw) {
        const auto reason = static_cast<ar::Reason>(raw);
        const std::string_view name = render::reason_name(reason);
        if (name == "Unknown") {
            break;
        }
        ++named;
        const bool hard = std::ranges::find(kHardReasons, reason) != kHardReasons.end();
        if (is_hard_reason(reason) != hard) {
            append_item(problems, fmt::format("{} should {}be hard", name, hard ? "" : "not "));
        }
        if (reason == ar::Reason::None) {
            if (!reason_label(reason).empty() || !reason_help(reason).empty()) {
                append_item(problems, "None has text");
            }
            continue;
        }
        if (reason_help(reason).empty()) {
            append_item(problems, fmt::format("{} has no help", name));
        }
        if ((hard || reason == ar::Reason::RequiredByShadows) && reason_label(reason).empty()) {
            append_item(problems, fmt::format("{} has no label", name));
        }
        if (hard == reason_option_label(reason).empty()) {
            append_item(problems,
                fmt::format("{} should {}have an option label", name, hard ? "" : "not "));
        }
    }
    if (named <= static_cast<unsigned>(ar::Reason::NeedsRt)) {
        append_item(problems, fmt::format("aurora names only {} reasons", named));
    }
    if (is_hard_reason(static_cast<ar::Reason>(named))) {
        append_item(problems, "an unknown reason locks rows");
    }
    return problems;
}

std::string describe(const RowStatus& status) {
    return fmt::format(
        "{{locked {}, label '{}', reason '{}'}}", status.locked, status.valueLabel, status.reason);
}

std::string expect_status(
    const RowStatus& actual, bool locked, std::string_view label, std::string_view reason) {
    if (actual.locked == locked && actual.valueLabel == label && actual.reason == reason) {
        return {};
    }
    return fmt::format("got {}, expected {}", describe(actual),
        describe({locked, std::string{label}, std::string{reason}}));
}

constexpr ar::FeatureSupport kSupported{true, ar::Reason::None};

std::string test_status_locked() {
    constexpr ar::FeatureSupport notBuilt{false, ar::Reason::NotImplemented};
    return expect_status(row_status(notBuilt, notBuilt, "Off", "Off", false, false), true,
        "Unavailable (not built yet)", reason_help(ar::Reason::NotImplemented));
}

std::string test_status_pending() {
    return expect_status(
        row_status(kSupported, kSupported, "High", "Off", true, false), false, "", "");
}

std::string test_status_arrow() {
    return expect_status(
        row_status(kSupported, {false, ar::Reason::LowVram}, "Ultra", "High", false, false), false,
        "Ultra \xE2\x86\x92 High", reason_help(ar::Reason::LowVram));
}

std::string test_status_required() {
    constexpr ar::FeatureSupport required{true, ar::Reason::RequiredByShadows};
    const std::string_view label = reason_label(ar::Reason::RequiredByShadows);
    const std::string_view help = reason_help(ar::Reason::RequiredByShadows);
    std::string problems;
    append_item(problems, expect_status(row_status(required, required, "Off", "On", false, false),
                              false, label, help));
    append_item(problems, expect_status(row_status(required, required, "On", "On", false, false),
                              false, label, help));
    append_item(problems, expect_status(row_status(required, required, "Off", "On", true, false),
                              false, label, help));
    return problems;
}

std::string test_status_applying() {
    return expect_status(row_status(kSupported, kSupported, "On", "On", false, true), false,
        "Applying\xE2\x80\xA6", "");
}

}  // namespace

const PresetValues& preset_values(GraphicsPreset preset) noexcept {
    if (!is_base_preset(preset)) {
        preset = GraphicsPreset::Original;
    }
    return kPresets[static_cast<size_t>(preset) - static_cast<size_t>(GraphicsPreset::Original)];
}

PresetValues read_preset_values(SettingsSource source) {
    PresetValues values{};
    visit_preset_fields([&](std::string_view, const auto& var, auto get) {
        get(values) = source == SettingsSource::Defaults ? var.getDefaultValue() : var.getValue();
    });
    return values;
}

void apply_preset(GraphicsPreset preset) {
    if (!is_base_preset(preset)) {
        preset = GraphicsPreset::Original;
    }
    const PresetValues& values = preset_values(preset);
    visit_preset_fields([&](std::string_view, auto& var, auto get) { var.setValue(get(values)); });
    getSettings().graphics.preset.setValue(preset);
}

void reset_to_preset() {
    apply_preset(base_preset());
    auto& graphics = getSettings().graphics;
#define DUSK_RESET_FIELD(member, field)                                                            \
    if (!is_preset_controlled("graphics." #member)) {                                              \
        graphics.member.setValue(graphics.member.getDefaultValue());                               \
    }
    DUSK_GRAPHICS_FIELDS(DUSK_RESET_FIELD)
#undef DUSK_RESET_FIELD
}

std::optional<bool> seed_generate_missing_mips() {
    auto& graphics = getSettings().graphics;
    const std::optional<bool> seeded = seeded_generate_missing_mips(
        graphics.preset.getValueForSave(), graphics.generateMissingMips.hasPersistedValue());
    if (seeded.has_value()) {
        // A launch override stays on top, as it would over a loaded value
        graphics.generateMissingMips.setValue(*seeded, false);
    }
    return seeded;
}

GraphicsPreset base_preset() {
    return getSettings().graphics.preset.getValue();
}

GraphicsPreset detect_preset() {
    const auto& graphics = getSettings().graphics;
    return detect_from(read_preset_values(), base_preset(), graphics.meshReplacement.getValue(),
        graphics.staticWorld.getValue());
}

std::string preset_display_name() {
    const GraphicsPreset base = base_preset();
    if (detect_preset() == GraphicsPreset::Custom) {
        return fmt::format("Custom ({})", preset_name(base));
    }
    return std::string{preset_name(base)};
}

GraphicsPreset recommend(const RecommendInput& input) noexcept {
    if (input.backend == BACKEND_NULL || !input.core) {
        return GraphicsPreset::Original;
    }
    if (input.profile == FEATURE_PROFILE_METAL32) {
        return GraphicsPreset::Low;
    }
    switch (input.adapter) {
    case ar::AdapterKind::Cpu:
        return GraphicsPreset::Original;
    case ar::AdapterKind::Discrete:
        break;
    default:
        return GraphicsPreset::Low;
    }
    if (input.backend != BACKEND_D3D12 && input.backend != BACKEND_VULKAN &&
        input.backend != BACKEND_METAL)
    {
        return GraphicsPreset::Low;
    }
    if (input.vramBytes > 0 && input.vramBytes < kHighPresetVramBytes) {
        return GraphicsPreset::Medium;
    }
    return GraphicsPreset::High;
}

GraphicsPreset recommended_preset() {
    const auto& c = ar::caps();
    return recommend({c.backend, c.core, c.profile, c.adapterKind, c.vramBytes});
}

template <typename T>
T reference_value(const config::ConfigVar<T>& var) {
    std::optional<T> reference;
    const PresetValues& base = preset_values(base_preset());
    visit_preset_fields([&](std::string_view, const auto& field, auto get) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, config::ConfigVar<T>>) {
            if (&field == &var) {
                reference = get(base);
            }
        }
    });
    return reference.has_value() ? *reference : var.getDefaultValue();
}

template bool reference_value(const config::ConfigVar<bool>&);
template int reference_value(const config::ConfigVar<int>&);
template float reference_value(const config::ConfigVar<float>&);
template GraphicsPreset reference_value(const config::ConfigVar<GraphicsPreset>&);
template GraphicsQuality reference_value(const config::ConfigVar<GraphicsQuality>&);
template GraphicsExposure reference_value(const config::ConfigVar<GraphicsExposure>&);
template GraphicsTonemap reference_value(const config::ConfigVar<GraphicsTonemap>&);
template GraphicsGi reference_value(const config::ConfigVar<GraphicsGi>&);
template GraphicsRtMode reference_value(const config::ConfigVar<GraphicsRtMode>&);
template GraphicsRtBackend reference_value(const config::ConfigVar<GraphicsRtBackend>&);
template GraphicsReflections reference_value(const config::ConfigVar<GraphicsReflections>&);
template GraphicsGameShadows reference_value(const config::ConfigVar<GraphicsGameShadows>&);
template GraphicsGodRays reference_value(const config::ConfigVar<GraphicsGodRays>&);
template GraphicsTerrainDetail reference_value(const config::ConfigVar<GraphicsTerrainDetail>&);
template GraphicsGrassShadows reference_value(const config::ConfigVar<GraphicsGrassShadows>&);
template GraphicsStaticWorld reference_value(const config::ConfigVar<GraphicsStaticWorld>&);

bool is_preset_controlled(std::string_view key) noexcept {
    return key == "graphics.preset" || std::ranges::find(kPresetKeys, key) != kPresetKeys.end();
}

bool differs_from_reference() {
    const auto& graphics = getSettings().graphics;
    bool differs = false;
#define DUSK_DIFFERS_FIELD(member, field)                                                          \
    differs = differs || !(graphics.member.getValue() == reference_value(graphics.member));
    DUSK_GRAPHICS_FIELDS(DUSK_DIFFERS_FIELD)
#undef DUSK_DIFFERS_FIELD
    return differs;
}

bool is_hard_reason(ar::Reason reason) noexcept {
    switch (reason) {
    case ar::Reason::NeedsCore:
    case ar::Reason::ColorBudget:
    case ar::Reason::NoFragmentStorage:
    case ar::Reason::NoVertexStorage:
    case ar::Reason::NoAtomic64:
    case ar::Reason::NoMdi:
    case ar::Reason::NeedsD3D12:
    case ar::Reason::NativeUnavailable:
    case ar::Reason::NativeDisabledAfterCrash:
    case ar::Reason::MsaaActive:
    case ar::Reason::NeedsRetainedScene:
    case ar::Reason::NeedsCompanions:
    case ar::Reason::BindingLimit:
    case ar::Reason::BuildDisabled:
    case ar::Reason::NotImplemented:
        return true;
    default:
        return false;
    }
}

std::string_view reason_label(ar::Reason reason) noexcept {
    switch (reason) {
    case ar::Reason::NativeDisabledAfterCrash:
        return "Disabled after a crash";
    case ar::Reason::BuildDisabled:
        return "Unavailable (not in this build)";
    case ar::Reason::NotImplemented:
        return "Unavailable (not built yet)";
    case ar::Reason::RequiredByShadows:
        return "On (required by real shadows)";
    default:
        return is_hard_reason(reason) ? "Unavailable" : "";
    }
}

std::string_view reason_option_label(ar::Reason reason) noexcept {
    switch (reason) {
    case ar::Reason::NativeDisabledAfterCrash:
        return "disabled after a crash";
    case ar::Reason::BuildDisabled:
        return "not in this build";
    case ar::Reason::NotImplemented:
        return "not built yet";
    default:
        return is_hard_reason(reason) ? "unavailable" : "";
    }
}

std::string_view reason_help(ar::Reason reason) noexcept {
    switch (reason) {
    case ar::Reason::None:
        return "";
    case ar::Reason::NeedsCore:
        return "Needs a GPU and driver with full WebGPU core features. Not available in "
               "compatibility mode.";
    case ar::Reason::ColorBudget:
        return "Your GPU cannot write enough render targets at once for this effect.";
    case ar::Reason::NoFragmentStorage:
        return "Needs storage buffers in pixel shaders, which this GPU does not support.";
    case ar::Reason::NoVertexStorage:
        return "Needs storage buffers in vertex shaders, which this GPU does not support.";
    case ar::Reason::NoAtomic64:
        return "Needs 64-bit atomic operations, which this GPU does not support.";
    case ar::Reason::NoMdi:
        return "Needs multi-draw indirect support, which this GPU does not provide.";
    case ar::Reason::NeedsD3D12:
        return "Only available with the Direct3D 12 graphics backend.";
    case ar::Reason::NativeUnavailable:
        return "Native ray tracing is not available on this GPU or driver.";
    case ar::Reason::NativeDisabledAfterCrash:
        return "Native ray tracing was turned off after a GPU crash. Use Re-enable Native Tracing "
               "to try again.";
    case ar::Reason::MsaaActive:
        return "Not compatible with multisample anti-aliasing.";
    case ar::Reason::NeedsRetainedScene:
        return "Needs the retained scene, which is not available on this device.";
    case ar::Reason::NeedsCompanions:
        return "Needs more texture slots than this GPU provides.";
    case ar::Reason::BindingLimit:
        return "Exceeds this GPU's resource binding limits.";
    case ar::Reason::BuildDisabled:
        return "This build of Dusklight does not include the enhanced renderer.";
    case ar::Reason::NotImplemented:
        return "This feature is not built yet. It will turn on in a future update.";
    case ar::Reason::NeedsSunShadows:
        return "Volumetric god rays need Shadow Quality above Off.";
    case ar::Reason::LowVram:
        return "Limited because your GPU has less than 6 GB of video memory, or the amount could "
               "not be detected.";
    case ar::Reason::NeedsPbr:
        return "Turn on PBR Lighting to use this.";
    case ar::Reason::RequiredByShadows:
        return "Cloud shadows stay on while real-time sun shadows are active, because they "
               "replace the game's own cloud shadows.";
    case ar::Reason::NeedsAo:
        return "Needs Ambient Occlusion.";
    case ar::Reason::NeedsRt:
        return "Needs an RT Mode other than Off.";
    }
    return "";
}

ar::FeatureSupport always_gate(uint8_t) noexcept {
    return {true, ar::Reason::None};
}

ar::FeatureSupport not_built_gate(uint8_t) noexcept {
    return {false, ar::Reason::NotImplemented};
}

ar::FeatureSupport terrain_detail_gate(uint8_t option) noexcept {
    switch (static_cast<GraphicsTerrainDetail>(option)) {
    case GraphicsTerrainDetail::Parallax:
        return ar::feature_support(ar::Feature::TerrainParallax);
    case GraphicsTerrainDetail::Tessellated:
        return ar::feature_support(ar::Feature::TerrainTessellation);
    default: {
        const ar::FeatureSupport parallax = ar::feature_support(ar::Feature::TerrainParallax);
        return parallax.supported ? parallax :
                                    ar::feature_support(ar::Feature::TerrainTessellation);
    }
    }
}

ar::FeatureSupport rt_backend_gate(uint8_t option) noexcept {
    if (static_cast<GraphicsRtBackend>(option) == GraphicsRtBackend::Dxr) {
        return ar::feature_support(ar::Feature::NativeTrace);
    }
    return ar::feature_support(ar::Feature::Rt);
}

ar::FeatureSupport native_reenable_gate(uint8_t option) noexcept {
    const ar::FeatureSupport support = ar::feature_support(ar::Feature::NativeTrace, option);
    // The crash lock is exactly when this button has work to do
    if (support.reason == ar::Reason::NativeDisabledAfterCrash) {
        return {true, ar::Reason::None};
    }
    return support;
}

uint8_t game_shadows_option(GraphicsGameShadows value) noexcept {
    switch (value) {
    case GraphicsGameShadows::Keep:
        return 0;
    case GraphicsGameShadows::Auto:
        return 1;
    case GraphicsGameShadows::Remove:
        return 2;
    }
    return 0;
}

bool terrain_ready_gate() noexcept {
    // feature_ready is true for an inactive feature, so this is the ready state of the active one
    return ar::feature_ready(ar::Feature::TerrainParallax) &&
           ar::feature_ready(ar::Feature::TerrainTessellation);
}

RowStatus row_status(ar::FeatureSupport row, ar::FeatureSupport option, std::string_view requested,
    std::string_view clamped, bool pending, bool notReady) {
    if (!row.supported && is_hard_reason(row.reason)) {
        return {true, std::string{reason_label(row.reason)}, std::string{reason_help(row.reason)}};
    }
    // Comes before pending: it follows the shadow settings, not this row, so it cannot flash
    if (option.reason == ar::Reason::RequiredByShadows ||
        row.reason == ar::Reason::RequiredByShadows)
    {
        return {false, std::string{reason_label(ar::Reason::RequiredByShadows)},
            std::string{reason_help(ar::Reason::RequiredByShadows)}};
    }
    // The clamped value is one push behind a change, so showing it would flash the old value
    if (pending) {
        return {};
    }
    if (requested != clamped) {
        const ar::Reason reason =
            !option.supported ? option.reason : (!row.supported ? row.reason : ar::Reason::None);
        return {false, fmt::format("{} \xE2\x86\x92 {}", requested, clamped),
            std::string{reason_help(reason)}};
    }
    if (notReady) {
        return {false, "Applying\xE2\x80\xA6", {}};
    }
    return {};
}

RowStatus evaluate_row(const RowGate& gate, uint8_t requestedOption, std::string_view requested,
    std::string_view clamped, bool requestedActive) {
    const ar::FeatureSupport row = gate.support(0);
    const ar::FeatureSupport option = requestedOption == 0 ? row : gate.support(requestedOption);
    const bool notReady = gate.ready != nullptr && requestedActive && !gate.ready();
    return row_status(row, option, requested, clamped, is_pending(), notReady);
}

void settings_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("presets.distinct", test_presets_distinct());
    check("presets.original_is_default", test_presets_original_is_default());
    check("presets.high_trimmed", test_presets_high_trimmed());
    check("presets.table", test_presets_table());
    check("presets.recommend", test_presets_recommend());
    check("presets.experimental_custom", test_presets_experimental_custom());
    check("presets.reference", test_presets_reference());
    check("presets.seed_missing_key", test_presets_seed_missing_key());
    check("reasons.complete", test_reasons_complete());
    check("status.locked", test_status_locked());
    check("status.pending", test_status_pending());
    check("status.arrow", test_status_arrow());
    check("status.required", test_status_required());
    check("status.applying", test_status_applying());
}

}  // namespace dusk::render
