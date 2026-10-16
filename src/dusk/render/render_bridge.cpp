#include "dusk/render/render_bridge.hpp"

#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/render/graphics_fields.inl"

#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace dusk::render {
namespace {
namespace ar = aurora::render;

constexpr borealis::Log RenderLog{"dusk::render"};
constexpr std::string_view kKeyPrefix = "graphics.";
constexpr size_t kMaxLoggedClampKeys = 12;

#define DUSK_GRAPHICS_ENUMS(X)                                                                     \
    X(GraphicsPreset, Preset)                                                                      \
    X(GraphicsQuality, Quality)                                                                    \
    X(GraphicsExposure, Exposure)                                                                  \
    X(GraphicsTonemap, Tonemap)                                                                    \
    X(GraphicsGi, GiMode)                                                                          \
    X(GraphicsRtMode, RtMode)                                                                      \
    X(GraphicsRtBackend, RtBackend)                                                                \
    X(GraphicsReflections, Reflections)                                                            \
    X(GraphicsGameShadows, GameShadows)                                                            \
    X(GraphicsGodRays, GodRays)                                                                    \
    X(GraphicsTerrainDetail, TerrainDetail)                                                        \
    X(GraphicsGrassShadows, GrassShadows)                                                          \
    X(GraphicsStaticWorld, StaticWorld)

#define DUSK_MIRROR_TYPE(DuskEnum, AuroraEnum)                                                     \
    static_assert(std::is_same_v<std::underlying_type_t<DuskEnum>, u8>);                           \
    static_assert(std::is_same_v<std::underlying_type_t<ar::AuroraEnum>, u8>);
DUSK_GRAPHICS_ENUMS(DUSK_MIRROR_TYPE)
#undef DUSK_MIRROR_TYPE

#define DUSK_MIRROR(DuskEnum, AuroraEnum, Name)                                                    \
    static_assert(static_cast<u8>(DuskEnum::Name) == static_cast<u8>(AuroraEnum::Name))

DUSK_MIRROR(GraphicsPreset, ar::Preset, Custom);
DUSK_MIRROR(GraphicsPreset, ar::Preset, Original);
DUSK_MIRROR(GraphicsPreset, ar::Preset, Low);
DUSK_MIRROR(GraphicsPreset, ar::Preset, Medium);
DUSK_MIRROR(GraphicsPreset, ar::Preset, High);
DUSK_MIRROR(GraphicsPreset, ar::Preset, Ultra);
DUSK_MIRROR(GraphicsQuality, ar::Quality, Off);
DUSK_MIRROR(GraphicsQuality, ar::Quality, Low);
DUSK_MIRROR(GraphicsQuality, ar::Quality, Medium);
DUSK_MIRROR(GraphicsQuality, ar::Quality, High);
DUSK_MIRROR(GraphicsQuality, ar::Quality, Ultra);
DUSK_MIRROR(GraphicsExposure, ar::Exposure, MatchLegacy);
DUSK_MIRROR(GraphicsExposure, ar::Exposure, Auto);
DUSK_MIRROR(GraphicsExposure, ar::Exposure, Manual);
DUSK_MIRROR(GraphicsTonemap, ar::Tonemap, PbrNeutral);
DUSK_MIRROR(GraphicsTonemap, ar::Tonemap, AgX);
DUSK_MIRROR(GraphicsGi, ar::GiMode, Off);
DUSK_MIRROR(GraphicsGi, ar::GiMode, Voxel);
DUSK_MIRROR(GraphicsGi, ar::GiMode, Ddgi);
DUSK_MIRROR(GraphicsRtMode, ar::RtMode, Off);
DUSK_MIRROR(GraphicsRtMode, ar::RtMode, Shadows);
DUSK_MIRROR(GraphicsRtMode, ar::RtMode, ShadowsAo);
DUSK_MIRROR(GraphicsRtMode, ar::RtMode, ShadowsAoReflections);
DUSK_MIRROR(GraphicsRtMode, ar::RtMode, PathTraceReference);
DUSK_MIRROR(GraphicsRtBackend, ar::RtBackend, Auto);
DUSK_MIRROR(GraphicsRtBackend, ar::RtBackend, Compute);
DUSK_MIRROR(GraphicsRtBackend, ar::RtBackend, Dxr);
DUSK_MIRROR(GraphicsReflections, ar::Reflections, Off);
DUSK_MIRROR(GraphicsReflections, ar::Reflections, Ssr);
DUSK_MIRROR(GraphicsReflections, ar::Reflections, SsrRt);
DUSK_MIRROR(GraphicsGameShadows, ar::GameShadows, Auto);
DUSK_MIRROR(GraphicsGameShadows, ar::GameShadows, Keep);
DUSK_MIRROR(GraphicsGameShadows, ar::GameShadows, Remove);
DUSK_MIRROR(GraphicsGodRays, ar::GodRays, Off);
DUSK_MIRROR(GraphicsGodRays, ar::GodRays, Auto);
DUSK_MIRROR(GraphicsGodRays, ar::GodRays, ScreenSpace);
DUSK_MIRROR(GraphicsGodRays, ar::GodRays, Volumetric);
DUSK_MIRROR(GraphicsTerrainDetail, ar::TerrainDetail, Off);
DUSK_MIRROR(GraphicsTerrainDetail, ar::TerrainDetail, Parallax);
DUSK_MIRROR(GraphicsTerrainDetail, ar::TerrainDetail, Tessellated);
DUSK_MIRROR(GraphicsGrassShadows, ar::GrassShadows, Off);
DUSK_MIRROR(GraphicsGrassShadows, ar::GrassShadows, Contact);
DUSK_MIRROR(GraphicsGrassShadows, ar::GrassShadows, Shell);
DUSK_MIRROR(GraphicsGrassShadows, ar::GrassShadows, Full);
DUSK_MIRROR(GraphicsStaticWorld, ar::StaticWorld, Off);
DUSK_MIRROR(GraphicsStaticWorld, ar::StaticWorld, Validate);
DUSK_MIRROR(GraphicsStaticWorld, ar::StaticWorld, On);

#undef DUSK_MIRROR

constexpr std::array kFeatures{
    ar::Feature::GBuffer,
    ar::Feature::Pbr,
    ar::Feature::Taa,
    ar::Feature::Ao,
    ar::Feature::Ssil,
    ar::Feature::Shadows,
    ar::Feature::GameShadowSuppression,
    ar::Feature::Gi,
    ar::Feature::Rt,
    ar::Feature::Reflections,
    ar::Feature::DeferredFog,
    ar::Feature::VolumetricFog,
    ar::Feature::GodRays,
    ar::Feature::Clouds,
    ar::Feature::CloudShadows,
    ar::Feature::Grass,
    ar::Feature::GrassTerrain,
    ar::Feature::TerrainParallax,
    ar::Feature::TerrainTessellation,
    ar::Feature::NativeTrace,
    ar::Feature::GpuFoliage,
    ar::Feature::GpuWeather,
    ar::Feature::GpuParticles,
    ar::Feature::GpuDeform,
    ar::Feature::MeshReplacement,
    ar::Feature::StaticWorld,
    ar::Feature::TextureMaterials,
    ar::Feature::StochasticTiling,
    ar::Feature::LegacyMatMaps,
};
static_assert(kFeatures.size() == ar::FeatureCount);
static_assert([] {
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        if (static_cast<size_t>(kFeatures[i]) != i) {
            return false;
        }
    }
    return true;
}());

#define DUSK_COUNT_FIELD(member, field) +1
constexpr size_t kFieldCount = 1 DUSK_GRAPHICS_FIELDS(DUSK_COUNT_FIELD);
#undef DUSK_COUNT_FIELD
// aurora's Settings has 62 members counting preset, and each needs exactly one key
constexpr size_t kAuroraSettingsMembers = 62;

uint64_t s_generation = 0;
uint64_t s_pushedGeneration = 0;
uint64_t s_pushes = 0;
uint64_t s_frames = 0;
ar::Settings s_lastRequested;
ar::Settings s_lastClamped;
uint64_t s_armedFrame = 0;
bool s_awaitingLatch = false;
uint32_t s_latency = 0;
std::string s_loggedClampKeys;

template <typename A, typename D>
A mirror_to(D value) noexcept {
    return static_cast<A>(static_cast<u8>(value));
}

template <typename D, typename A>
D mirror_from(A value) noexcept {
    const auto raw = static_cast<u8>(value);
    // aurora can append an option before dusk has a config value for it
    if (raw > static_cast<u8>(config::ConfigEnumRange<D>::max)) {
        return config::ConfigEnumRange<D>::min;
    }
    return static_cast<D>(raw);
}

template <typename>
inline constexpr bool kNoConversion = false;

template <typename D, typename S>
D convert(const S& value, [[maybe_unused]] const S& fallback) noexcept {
    if constexpr (std::is_floating_point_v<S>) {
        static_assert(std::is_same_v<D, S>);
        return std::isfinite(value) ? value : fallback;
    } else if constexpr (std::is_same_v<D, S>) {
        return value;
    } else if constexpr (std::is_enum_v<S>) {
        return to_aurora(value);
    } else if constexpr (std::is_same_v<S, int> && std::is_same_v<D, uint32_t>) {
        return static_cast<uint32_t>(std::max(value, 0));
    } else if constexpr (std::is_same_v<S, int> && std::is_same_v<D, uint16_t>) {
        return static_cast<uint16_t>(std::clamp(value, 0, 65535));
    } else if constexpr (std::is_same_v<S, int> && std::is_same_v<D, float>) {
        return static_cast<float>(std::max(value, 0));
    } else {
        static_assert(kNoConversion<D>, "graphics field has no conversion");
    }
}

template <typename T>
const T& read(const config::ConfigVar<T>& var, SettingsSource source) noexcept {
    return source == SettingsSource::Defaults ? var.getDefaultValue() : var.getValue();
}

template <typename T>
std::string field_text(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<T>) {
        return fmt::format("{:.9g}", value);
    } else if constexpr (std::is_enum_v<T>) {
        return fmt::format("{}", static_cast<unsigned>(value));
    } else {
        return fmt::format("{}", value);
    }
}

template <typename Fn>
void visit_fields(Fn&& fn) {
    auto& graphics = getSettings().graphics;
    fn(std::string_view{"graphics.preset"}, graphics.preset,
        [](auto& settings) -> auto& { return settings.preset; });
#define DUSK_VISIT_FIELD(member, field)                                                            \
    fn(std::string_view{"graphics." #member}, graphics.member,                                     \
        [](auto& settings) -> auto& { return settings.field; });
    DUSK_GRAPHICS_FIELDS(DUSK_VISIT_FIELD)
#undef DUSK_VISIT_FIELD
}

std::vector<std::string_view> differing_keys(const ar::Settings& a, const ar::Settings& b) {
    std::vector<std::string_view> keys;
    visit_fields([&](std::string_view key, const auto&, auto get) {
        if (field_text(get(a)) != field_text(get(b))) {
            keys.push_back(key);
        }
    });
    return keys;
}

std::string join_keys(const std::vector<std::string_view>& keys, size_t limit) {
    std::string joined;
    for (size_t i = 0; i < keys.size() && i < limit; ++i) {
        if (i != 0) {
            joined += ", ";
        }
        joined += keys[i].substr(kKeyPrefix.size());
    }
    if (keys.size() > limit) {
        joined += ", \xE2\x80\xA6";
    }
    return joined;
}

void append_item(std::string& list, std::string_view item) {
    if (!list.empty()) {
        list += ", ";
    }
    list += item;
}

// Compares the config text, so dusk's own sanitising (nan, negative counts) is reported too
void log_clamps(const ar::Settings& clamped) {
    std::vector<std::string_view> keys;
    visit_fields([&](std::string_view key, const auto& var, auto get) {
        if (field_text(var.getValue()) != field_text(get(clamped))) {
            keys.push_back(key);
        }
    });
    std::string all = join_keys(keys, keys.size());
    if (all == s_loggedClampKeys) {
        return;
    }
    s_loggedClampKeys = std::move(all);
    if (keys.empty()) {
        RenderLog.info("Renderer clamps no settings");
        return;
    }
    RenderLog.info("Renderer clamps {} {}: {}", keys.size(),
        keys.size() == 1 ? "setting" : "settings", join_keys(keys, kMaxLoggedClampKeys));
}

template <typename D>
bool round_trips() noexcept {
    using A = decltype(to_aurora(D{}));
    constexpr auto last = static_cast<unsigned>(config::ConfigEnumRange<D>::max);
    for (unsigned raw = 0; raw <= last; ++raw) {
        const auto value = static_cast<D>(raw);
        if (from_aurora(to_aurora(value)) != value) {
            return false;
        }
    }
    return from_aurora(static_cast<A>(last + 1)) == config::ConfigEnumRange<D>::min;
}

std::string test_enum_mirror() {
    std::string failing;
#define DUSK_ROUND_TRIP(DuskEnum, AuroraEnum)                                                      \
    if (!round_trips<DuskEnum>()) {                                                                \
        append_item(failing, #DuskEnum);                                                           \
    }
    DUSK_GRAPHICS_ENUMS(DUSK_ROUND_TRIP)
#undef DUSK_ROUND_TRIP
    return failing.empty() ? std::string{} :
                             "round trip or out-of-range fallback fails for " + failing;
}

std::string test_defaults_match_aurora() {
    const ar::Settings defaults = build_requested(SettingsSource::Defaults);
    const ar::Settings reference{};
    if (same_settings(defaults, reference)) {
        return {};
    }
    return "defaults differ from Settings{} in " +
           join_keys(differing_keys(defaults, reference), kFieldCount);
}

std::string test_field_coverage() {
    const ar::Settings probe{};
    std::vector<std::string_view> keys;
    std::vector<const void*> members;
    std::string problems;
    visit_fields([&](std::string_view key, auto& var, auto get) {
        if (std::ranges::find(keys, key) != keys.end()) {
            append_item(problems, fmt::format("{} listed twice", key));
        }
        keys.push_back(key);
        const void* member = &get(probe);
        if (std::ranges::find(members, member) != members.end()) {
            append_item(problems, fmt::format("{} maps to an aurora member already used", key));
        }
        members.push_back(member);
        if (key != var.getName()) {
            append_item(problems, fmt::format("{} is named {}", key, var.getName()));
        }
        if (config::GetConfigVar(key) != &var) {
            append_item(problems, fmt::format("{} is not registered", key));
        }
    });
    config::EnumerateRegistered([&](config::ConfigVarBase& var) {
        const std::string_view name = var.getName();
        if (name.starts_with(kKeyPrefix) && std::ranges::find(keys, name) == keys.end()) {
            append_item(problems, fmt::format("{} is not in the field table", name));
        }
    });
    if (keys.size() != kAuroraSettingsMembers) {
        append_item(
            problems, fmt::format("{} keys, expected {}", keys.size(), kAuroraSettingsMembers));
    }
    return problems;
}

std::string test_push_idempotent() {
    const ar::Settings again = ar::apply_settings(s_lastRequested);
    if (same_settings(again, s_lastClamped)) {
        return {};
    }
    return "pushing the last request again changes " +
           join_keys(differing_keys(again, s_lastClamped), kFieldCount);
}

bool latch_overdue() noexcept {
    return s_awaitingLatch && s_frames - s_armedFrame > 1;
}

std::string test_latch_latency() {
    if (latch_overdue()) {
        return fmt::format("change not latched after {} frames", s_frames - s_armedFrame);
    }
    return s_latency == 1 ? std::string{} :
                            fmt::format("latency is {} frames, expected 1", s_latency);
}

void push() {
    ++s_pushes;
    const ar::Settings& effective = ar::effective_settings();
    if (s_awaitingLatch && same_settings(effective, s_lastClamped)) {
        s_latency = static_cast<uint32_t>(s_frames - s_armedFrame);
        s_awaitingLatch = false;
    }
    const ar::Settings requested = build_requested();
    const ar::Settings clamped = ar::apply_settings(requested);
    const bool first = s_pushes == 1;
    const bool clampedChanged = first || !same_settings(clamped, s_lastClamped);
    if (clampedChanged) {
        // Only a change the latch still has to apply is timed, so an old result never reads as new
        s_awaitingLatch = !same_settings(effective, clamped);
        if (s_awaitingLatch) {
            s_armedFrame = s_frames;
            s_latency = 0;
        }
    }
    if (clampedChanged || is_pending() || !same_settings(requested, s_lastRequested)) {
        log_clamps(clamped);
    }
    s_lastRequested = requested;
    s_lastClamped = clamped;
    s_pushedGeneration = s_generation;
}

std::string test_features_named() {
    std::string problems;
    std::vector<std::string_view> names;
    const auto check_name = [&](std::string_view kind, unsigned raw, std::string_view name) {
        if (name.empty() || name == "Unknown") {
            append_item(problems, fmt::format("{} {} has no name", kind, raw));
        } else if (std::ranges::find(names, name) != names.end()) {
            append_item(problems, fmt::format("{} name {} is not unique", kind, name));
        }
        names.push_back(name);
    };

    for (const auto feature : all_features()) {
        check_name("feature", static_cast<unsigned>(feature), render::feature_name(feature));
    }
    size_t namedFeatures = 0;
    while (namedFeatures < 256 &&
           ar::feature_name(static_cast<ar::Feature>(namedFeatures)) != "Unknown")
    {
        ++namedFeatures;
    }
    if (namedFeatures != all_features().size()) {
        append_item(problems, fmt::format("aurora names {} features, all_features() lists {}",
                                  namedFeatures, all_features().size()));
    }

    names.clear();
    size_t namedReasons = 0;
    for (unsigned raw = 0; raw < 256; ++raw) {
        const std::string_view name = render::reason_name(static_cast<ar::Reason>(raw));
        if (name == "Unknown") {
            break;
        }
        check_name("reason", raw, name);
        ++namedReasons;
    }
    if (namedReasons <= static_cast<size_t>(ar::Reason::NeedsRt)) {
        append_item(problems, fmt::format("aurora names only {} reasons", namedReasons));
    }
    return problems;
}

}  // namespace

#define DUSK_DEFINE_CONVERSIONS(DuskEnum, AuroraEnum)                                              \
    ar::AuroraEnum to_aurora(DuskEnum value) noexcept {                                            \
        return mirror_to<ar::AuroraEnum>(value);                                                   \
    }                                                                                              \
    DuskEnum from_aurora(ar::AuroraEnum value) noexcept {                                          \
        return mirror_from<DuskEnum>(value);                                                       \
    }
DUSK_GRAPHICS_ENUMS(DUSK_DEFINE_CONVERSIONS)
#undef DUSK_DEFINE_CONVERSIONS

void initialize() {
    present();
    size_t supported = 0;
    for (const auto feature : all_features()) {
        if (ar::feature_support(feature).supported) {
            ++supported;
        }
    }
    RenderLog.info("Renderer bridge: preset {}, {} of {} features supported",
        preset_name(getSettings().graphics.preset.getValue()), supported, all_features().size());
}

void present() {
    ZoneScopedN("dusk::render::present");
    push();
    ++s_frames;
}

void push_now() {
    push();
}

void mark_dirty() noexcept {
    ++s_generation;
}

bool is_pending() noexcept {
    return s_generation != s_pushedGeneration;
}

ar::Settings build_requested(SettingsSource source) {
    ar::Settings settings;
    visit_fields([&](std::string_view, const auto& var, auto get) {
        auto& out = get(settings);
        out = convert<std::remove_reference_t<decltype(out)>>(
            read(var, source), var.getDefaultValue());
    });
    return settings;
}

const ar::Settings& last_requested() noexcept {
    return s_lastRequested;
}

const ar::Settings& last_clamped() noexcept {
    return s_lastClamped;
}

uint64_t push_count() noexcept {
    return s_pushes;
}

uint32_t last_latch_latency() noexcept {
    return s_latency;
}

bool same_settings(const ar::Settings& a, const ar::Settings& b) noexcept {
    bool same = true;
    visit_fields([&](std::string_view, const auto&, auto get) { same = same && get(a) == get(b); });
    return same;
}

std::vector<FieldText> describe_fields() {
    const ar::Settings& effective = ar::effective_settings();
    std::vector<FieldText> fields;
    fields.reserve(kFieldCount);
    visit_fields([&](std::string_view key, const auto& var, auto get) {
        fields.push_back({key, field_text(var.getValue()), field_text(get(s_lastClamped)),
            field_text(get(effective))});
    });
    return fields;
}

std::string cvar_arguments() {
    std::string arguments;
    visit_fields([&](std::string_view key, const auto& var, auto) {
        if (var.getValue() == var.getDefaultValue()) {
            return;
        }
        if (!arguments.empty()) {
            arguments += ' ';
        }
        arguments += fmt::format("--cvar {}={}", key, field_text(var.getValue()));
    });
    return arguments;
}

std::string_view preset_name(GraphicsPreset preset) noexcept {
    switch (preset) {
    case GraphicsPreset::Custom:
        return "Custom";
    case GraphicsPreset::Original:
        return "Original";
    case GraphicsPreset::Low:
        return "Low";
    case GraphicsPreset::Medium:
        return "Medium";
    case GraphicsPreset::High:
        return "High";
    case GraphicsPreset::Ultra:
        return "Ultra";
    }
    return "Unknown";
}

std::span<const ar::Feature> all_features() noexcept {
    return kFeatures;
}

std::string_view feature_name(ar::Feature feature) noexcept {
    return ar::feature_name(feature);
}

std::string_view reason_name(ar::Reason reason) noexcept {
    return ar::reason_name(reason);
}

void bridge_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("bridge.enum_mirror", test_enum_mirror());
    check("bridge.defaults_match_aurora", test_defaults_match_aurora());
    check("bridge.field_coverage", test_field_coverage());
    check("bridge.push_idempotent", test_push_idempotent());
    if (s_latency == 0 && !latch_overdue()) {
        report.skipped.push_back("bridge.latch_latency: no settings change has been latched yet");
    } else {
        check("bridge.latch_latency", test_latch_latency());
    }
    check("bridge.features_named", test_features_named());
}

}  // namespace dusk::render
