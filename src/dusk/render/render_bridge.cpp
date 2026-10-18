#include "dusk/render/render_bridge.hpp"

#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/render/draw_tags.hpp"
#include "dusk/render/graphics_fields.inl"
#include "dusk/render/hints.hpp"
#include "dusk/render/render_settings.hpp"
#include "f_op/f_op_camera_mng.h"

#include <aurora/gfx.hpp>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

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

// The preset member carries the detected preset, so editing a preset-controlled key requests Custom
template <typename T>
T requested_value(const config::ConfigVar<T>& var, SettingsSource source) {
    if constexpr (std::is_same_v<T, GraphicsPreset>) {
        return source == SettingsSource::Defaults ? GraphicsPreset::Original : detect_preset();
    } else {
        return read(var, source);
    }
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
        if (field_text(requested_value(var, SettingsSource::Current)) != field_text(get(clamped))) {
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
    RenderLog.info("Renderer bridge: preset {}, {} of {} features supported", preset_display_name(),
        supported, all_features().size());
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
            requested_value(var, source), var.getDefaultValue());
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
    // Requested stays the saved text; the preset is compared as the detected tag aurora was sent
    visit_fields([&](std::string_view key, const auto& var, auto get) {
        fields.push_back({key, field_text(var.getValue()), field_text(get(s_lastClamped)),
            field_text(get(effective)), field_text(requested_value(var, SettingsSource::Current))});
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

namespace {

static_assert(AURORA_STAGE_SCENE_BEGIN == static_cast<int>(ar::Stage::SceneBegin) + 1);
static_assert(AURORA_STAGE_OPAQUE_DONE == static_cast<int>(ar::Stage::OpaqueDone) + 1);
static_assert(AURORA_STAGE_PRE_POST == static_cast<int>(ar::Stage::PrePost) + 1);
static_assert(AURORA_STAGE_SCENE_END == static_cast<int>(ar::Stage::SceneEnd) + 1);
static_assert(AURORA_STAGE_BEFORE_HUD == static_cast<int>(ar::Stage::BeforeHud) + 1);
static_assert(AURORA_STAGE_FRAME_END == static_cast<int>(ar::Stage::FrameEnd) + 1);
static_assert(static_cast<int>(ar::Stage::FrameEnd) == 5);

constexpr uint8_t stage_bit(ar::Stage stage) noexcept {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(stage));
}
constexpr uint8_t kAllMarkerBits = 0x3F;
constexpr uint8_t kHudOnlyBits = stage_bit(ar::Stage::BeforeHud) | stage_bit(ar::Stage::FrameEnd);
constexpr uint32_t kSingleCutEpoch = 1;
constexpr uint16_t kFirstHintId = GX_AURORA_HINT_FIRST;
constexpr uint16_t kLastHintId = GX_AURORA_HINT_LAST;
constexpr size_t kHintIdCount = kLastHintId - kFirstHintId + 1;
constexpr std::array<uint16_t, 4> kPainterHintIds{GX_AURORA_RENDER_CAMERA, GX_AURORA_RENDER_STAGE,
    GX_AURORA_RENDER_SCOPE_PUSH, GX_AURORA_RENDER_SCOPE_POP};

enum class FramePattern : u8 { Scene, HudOnly, Other };

struct MissingStages {
    std::array<ar::Stage, 3> stages{};
    uint8_t count = 0;
};

class FrameTracker {
public:
    bool accept_main_camera() noexcept {
        if (m_camera || m_stages != 0) {
            return false;
        }
        m_camera = true;
        return true;
    }

    bool accept_stage(ar::Stage stage) noexcept {
        const int value = static_cast<int>(stage);
        if (stage > ar::Stage::FrameEnd || value <= m_last ||
            (stage <= ar::Stage::SceneEnd && !m_camera))
        {
            return false;
        }
        m_stages |= stage_bit(stage);
        m_last = static_cast<int8_t>(value);
        return true;
    }

    MissingStages close_scene() const noexcept {
        MissingStages missing;
        if (!m_camera || (m_stages & stage_bit(ar::Stage::SceneBegin)) == 0) {
            return missing;
        }
        for (int value = std::max<int>(m_last + 1, static_cast<int>(ar::Stage::OpaqueDone));
            value <= static_cast<int>(ar::Stage::SceneEnd); ++value)
        {
            missing.stages[missing.count++] = static_cast<ar::Stage>(value);
        }
        return missing;
    }

    FramePattern pattern(uint32_t openScopes) const noexcept {
        if (openScopes != 0) {
            return FramePattern::Other;
        }
        if (m_camera && m_stages == kAllMarkerBits) {
            return FramePattern::Scene;
        }
        if (!m_camera && m_stages == kHudOnlyBits) {
            return FramePattern::HudOnly;
        }
        return FramePattern::Other;
    }

    bool main_camera() const noexcept { return m_camera; }
    uint8_t stages() const noexcept { return m_stages; }
    bool operator==(const FrameTracker&) const = default;

private:
    uint8_t m_stages = 0;
    int8_t m_last = -1;
    bool m_camera = false;
};

// A marker inside a tag scope still goes out, because refusing it would lose a whole stage window
bool count_open_scopes(HintStats& stats, uint32_t openScopes) noexcept {
    if (openScopes == 0) {
        return false;
    }
    ++stats.openAtMarker;
    return true;
}

FrameTracker s_frame;
HintStats s_hintStats;
bool s_hintsOn = false;
bool s_painterOpen = false;
bool s_resetRequested = false;
bool s_pausedAtPrePost = false;
uint32_t s_painterFrame = 0;
uint32_t s_cutEpoch = 0;
cXyz s_lastEye(0.0f, 0.0f, 0.0f);
f32 s_lastFovy = 0.0f;
std::array<ar::HintCounters, kHintIdCount> s_baseline{};
std::array<bool, ar::StageCount> s_warnedStageRefusal{};
bool s_warnedCameraRefusal = false;
bool s_warnedOpenAtMarker = false;
bool s_warnedOther = false;

std::string_view stage_abbreviation(ar::Stage stage) noexcept {
    switch (stage) {
    case ar::Stage::SceneBegin:
        return "SB";
    case ar::Stage::OpaqueDone:
        return "OD";
    case ar::Stage::PrePost:
        return "PP";
    case ar::Stage::SceneEnd:
        return "SE";
    case ar::Stage::BeforeHud:
        return "BH";
    case ar::Stage::FrameEnd:
        return "FE";
    case ar::Stage::AfterFrame:
        return "AF";
    }
    return "??";
}

std::string stage_list(uint8_t bits) {
    std::string list;
    for (size_t i = 0; i < ar::StageCount; ++i) {
        const auto stage = static_cast<ar::Stage>(i);
        if ((bits & stage_bit(stage)) != 0) {
            if (!list.empty()) {
                list += ' ';
            }
            list += stage_abbreviation(stage);
        }
    }
    return list.empty() ? std::string{"none"} : list;
}

std::string_view hint_id_name(uint16_t id) noexcept {
    switch (id) {
    case GX_AURORA_SET_ARRAY_IMMUTABLE:
        return "SET_ARRAY_IMMUTABLE";
    case GX_AURORA_RELEASE_ARRAY_IMMUTABLE:
        return "RELEASE_ARRAY_IMMUTABLE";
    case GX_AURORA_RENDER_CAMERA:
        return "CAMERA";
    case GX_AURORA_RENDER_OBJECT_BEGIN:
        return "OBJECT_BEGIN";
    case GX_AURORA_RENDER_OBJECT_END:
        return "OBJECT_END";
    case GX_AURORA_RENDER_SCOPE_PUSH:
        return "SCOPE_PUSH";
    case GX_AURORA_RENDER_SCOPE_POP:
        return "SCOPE_POP";
    case GX_AURORA_RENDER_STAGE:
        return "STAGE";
    case GX_AURORA_RENDER_LIGHTS:
        return "LIGHTS";
    case GX_AURORA_RENDER_ENVIRONMENT:
        return "ENVIRONMENT";
    case GX_AURORA_RENDER_MATERIAL:
        return "MATERIAL";
    case GX_AURORA_RENDER_SKELETON:
        return "SKELETON";
    case GX_AURORA_RENDER_SCENE_EPOCH:
        return "SCENE_EPOCH";
    case GX_AURORA_RENDER_TEV_POLICY:
        return "TEV_POLICY";
    case GX_AURORA_RENDER_INFLUENCERS:
        return "INFLUENCERS";
    case GX_AURORA_RENDER_WIND_SOURCES:
        return "WIND_SOURCES";
    case GX_AURORA_RENDER_GRASS_PATCHES:
        return "GRASS_PATCHES";
    case GX_AURORA_RENDER_SKY:
        return "SKY";
    case GX_AURORA_RENDER_FOG_VOLUMES:
        return "FOG_VOLUMES";
    case GX_AURORA_GPU_UPLOAD:
        return "GPU_UPLOAD";
    case GX_AURORA_GPU_RESERVE:
        return "GPU_RESERVE";
    case GX_AURORA_GPU_DISPATCH:
        return "GPU_DISPATCH";
    case GX_AURORA_LOAD_ARRAY_GPU:
        return "LOAD_ARRAY_GPU";
    case GX_AURORA_GPU_UPDATE_PERSISTENT:
        return "GPU_UPDATE_PERSISTENT";
    case GX_AURORA_GPU_CREATE_SYSTEM:
        return "GPU_CREATE_SYSTEM";
    case GX_AURORA_GPU_DESTROY_SYSTEM:
        return "GPU_DESTROY_SYSTEM";
    default:
        return "reserved";
    }
}

// aurora's counters are process totals, so everything here is relative to the last applied reset
ar::HintCounters hint_delta(uint16_t id) noexcept {
    const ar::HintCounters now = ar::hint_counters(id);
    const ar::HintCounters& base = s_baseline[id - kFirstHintId];
    return {now.decoded - base.decoded, now.skipped - base.skipped, now.malformed - base.malformed,
        now.late - base.late, now.bytes - base.bytes};
}

std::string counter_text(const ar::HintCounters& counters) {
    return fmt::format("decoded {} skipped {} malformed {} late {} bytes {}", counters.decoded,
        counters.skipped, counters.malformed, counters.late, counters.bytes);
}

// Built only when a warning fires, because markers go out several times every frame
std::string hint_subject(std::optional<ar::Stage> stage) {
    return stage ? fmt::format("stage {}", ar::stage_name(*stage)) : std::string{"main camera"};
}

void refuse(std::optional<ar::Stage> stage) {
    ++s_hintStats.refused;
    bool& warned =
        stage ? s_warnedStageRefusal[static_cast<size_t>(*stage)] : s_warnedCameraRefusal;
    if (!std::exchange(warned, true)) {
        RenderLog.warn("render hints: refused {} in frame {}", hint_subject(stage),
            aurora::gfx::current_frame());
    }
}

void note_open_scopes(std::optional<ar::Stage> stage) {
    const uint32_t open = open_tag_scopes();
    if (count_open_scopes(s_hintStats, open) && !std::exchange(s_warnedOpenAtMarker, true)) {
        RenderLog.warn("render hints: {} sent with {} open tag scopes in frame {}",
            hint_subject(stage), open, aurora::gfx::current_frame());
    }
}

void close_frame() {
    ++s_hintStats.frames;
    const uint32_t open = open_tag_scopes();
    switch (s_frame.pattern(open)) {
    case FramePattern::Scene:
        ++s_hintStats.sceneFrames;
        if (s_pausedAtPrePost) {
            ++s_hintStats.pausedSceneFrames;
        }
        break;
    case FramePattern::HudOnly:
        ++s_hintStats.hudOnlyFrames;
        break;
    case FramePattern::Other:
        ++s_hintStats.otherFrames;
        if (!std::exchange(s_warnedOther, true)) {
            RenderLog.warn("render hints: frame {} sent stages {}, open tag scopes {}",
                s_painterFrame, stage_list(s_frame.stages()), open);
        }
        break;
    }
    s_hintStats.lastStages = s_frame.stages();
    s_painterOpen = false;
}

void emit_stage(ar::Stage stage) {
    note_open_scopes(stage);
    if (!s_frame.accept_stage(stage)) {
        refuse(stage);
        return;
    }
    if (stage == ar::Stage::PrePost) {
        s_pausedAtPrePost = dComIfGp_isPauseFlag() != 0;
    }
    GXAuroraStage(static_cast<AuroraRenderStage>(static_cast<int>(stage) + 1));
    ++s_hintStats.stageHints;
    if (stage == ar::Stage::FrameEnd) {
        close_frame();
    }
}

FrameTracker tracker_after(std::initializer_list<ar::Stage> stages, bool camera) {
    FrameTracker tracker;
    if (camera) {
        tracker.accept_main_camera();
    }
    for (const auto stage : stages) {
        tracker.accept_stage(stage);
    }
    return tracker;
}

void accept_all(
    FrameTracker& tracker, std::initializer_list<ar::Stage> stages, std::string& problems) {
    for (const auto stage : stages) {
        if (!tracker.accept_stage(stage)) {
            append_item(problems, fmt::format("{} refused", ar::stage_name(stage)));
        }
    }
}

std::string test_scene_frame() {
    std::string problems;
    FrameTracker tracker;
    if (!tracker.accept_main_camera()) {
        append_item(problems, "camera refused");
    }
    accept_all(tracker,
        {ar::Stage::SceneBegin, ar::Stage::OpaqueDone, ar::Stage::PrePost, ar::Stage::SceneEnd,
            ar::Stage::BeforeHud, ar::Stage::FrameEnd},
        problems);
    if (tracker.pattern(0) != FramePattern::Scene) {
        append_item(problems, "pattern is not Scene");
    }
    return problems;
}

std::string test_pause_frame() {
    std::string problems;
    FrameTracker tracker;
    if (!tracker.accept_main_camera()) {
        append_item(problems, "camera refused");
    }
    accept_all(
        tracker, {ar::Stage::SceneBegin, ar::Stage::OpaqueDone, ar::Stage::PrePost}, problems);
    const MissingStages missing = tracker.close_scene();
    if (missing.count != 1 || missing.stages[0] != ar::Stage::SceneEnd) {
        append_item(problems, fmt::format("close_scene returns {} stages", missing.count));
    }
    accept_all(tracker, {ar::Stage::SceneEnd, ar::Stage::BeforeHud, ar::Stage::FrameEnd}, problems);
    if (tracker.pattern(0) != FramePattern::Scene) {
        append_item(problems, "pattern is not Scene");
    }
    if (tracker.close_scene().count != 0) {
        append_item(problems, "a second close_scene returns stages");
    }
    return problems;
}

std::string test_camera_less_frame() {
    std::string problems;
    FrameTracker tracker;
    if (tracker.close_scene().count != 0) {
        append_item(problems, "close_scene returns stages without a camera");
    }
    accept_all(tracker, {ar::Stage::BeforeHud, ar::Stage::FrameEnd}, problems);
    if (tracker.close_scene().count != 0) {
        append_item(problems, "close_scene returns stages after FE");
    }
    if (tracker.pattern(0) != FramePattern::HudOnly) {
        append_item(problems, "pattern is not HudOnly");
    }
    return problems;
}

std::string test_refusals() {
    std::string problems;
    const auto refused_stage = [&](FrameTracker tracker, ar::Stage stage, std::string_view what) {
        const FrameTracker before = tracker;
        if (tracker.accept_stage(stage) || !(tracker == before)) {
            append_item(problems, what);
        }
    };
    const auto refused_camera = [&](FrameTracker tracker, std::string_view what) {
        const FrameTracker before = tracker;
        if (tracker.accept_main_camera() || !(tracker == before)) {
            append_item(problems, what);
        }
    };
    const FrameTracker full =
        tracker_after({ar::Stage::SceneBegin, ar::Stage::OpaqueDone, ar::Stage::PrePost,
                          ar::Stage::SceneEnd, ar::Stage::BeforeHud, ar::Stage::FrameEnd},
            true);
    refused_stage(FrameTracker{}, ar::Stage::SceneBegin, "SB before the camera accepted");
    refused_camera(tracker_after({ar::Stage::SceneBegin}, true), "camera after SB accepted");
    refused_camera(tracker_after({ar::Stage::BeforeHud}, false), "camera after BH accepted");
    refused_camera(tracker_after({}, true), "second camera accepted");
    refused_stage(tracker_after({ar::Stage::SceneBegin, ar::Stage::OpaqueDone}, true),
        ar::Stage::OpaqueDone, "OD twice accepted");
    refused_stage(tracker_after({ar::Stage::SceneBegin, ar::Stage::OpaqueDone, ar::Stage::PrePost,
                                    ar::Stage::SceneEnd},
                      true),
        ar::Stage::PrePost, "PP after SE accepted");
    refused_stage(tracker_after({}, true), ar::Stage::AfterFrame, "AfterFrame accepted");
    for (size_t i = 0; i < ar::StageCount; ++i) {
        const auto stage = static_cast<ar::Stage>(i);
        refused_stage(full, stage, fmt::format("{} after FE accepted", ar::stage_name(stage)));
    }
    if (full.stages() != kAllMarkerBits || !full.main_camera()) {
        append_item(problems, "a full frame is not recorded");
    }
    return problems;
}

std::string test_other_patterns() {
    std::string problems;
    const FrameTracker partial =
        tracker_after({ar::Stage::SceneBegin, ar::Stage::BeforeHud, ar::Stage::FrameEnd}, true);
    if (partial.pattern(0) != FramePattern::Other) {
        append_item(problems, "camera SB BH FE is not Other");
    }
    if (tracker_after({}, true).pattern(0) != FramePattern::Other) {
        append_item(problems, "camera only is not Other");
    }
    const FrameTracker hud = tracker_after({ar::Stage::BeforeHud, ar::Stage::FrameEnd}, false);
    if (hud.pattern(1) != FramePattern::Other) {
        append_item(problems, "BH FE with an open tag scope is not Other");
    }
    return problems;
}

std::string test_open_at_marker() {
    std::string problems;
    HintStats stats;
    FrameTracker tracker;
    if (!count_open_scopes(stats, 2) || !tracker.accept_main_camera()) {
        append_item(problems, "camera with open scopes refused or not counted");
    }
    if (count_open_scopes(stats, 0) || !tracker.accept_stage(ar::Stage::SceneBegin)) {
        append_item(problems, "marker without open scopes counted or refused");
    }
    if (!count_open_scopes(stats, 1) || !tracker.accept_stage(ar::Stage::OpaqueDone)) {
        append_item(problems, "marker with an open scope refused or not counted");
    }
    if (stats.openAtMarker != 2 || stats.refused != 0) {
        append_item(problems, fmt::format("counted {} open at marker, {} refused",
                                  stats.openAtMarker, stats.refused));
    }
    return problems;
}

std::string test_live_frames() {
    const HintStats& stats = s_hintStats;
    std::string problems;
    if (stats.otherFrames != 0) {
        append_item(problems, fmt::format("{} other frames", stats.otherFrames));
    }
    if (stats.refused != 0) {
        append_item(problems, fmt::format("{} refused", stats.refused));
    }
    if (stats.frames != stats.sceneFrames + stats.hudOnlyFrames) {
        append_item(problems, fmt::format("{} frames, {} scene and {} camera-less", stats.frames,
                                  stats.sceneFrames, stats.hudOnlyFrames));
    }
    return problems;
}

std::string test_aurora_agrees() {
    std::string problems;
    const ar::HintCounters camera = hint_delta(GX_AURORA_RENDER_CAMERA);
    const ar::HintCounters stage = hint_delta(GX_AURORA_RENDER_STAGE);
    const ar::HintCounters push = hint_delta(GX_AURORA_RENDER_SCOPE_PUSH);
    const ar::HintCounters pop = hint_delta(GX_AURORA_RENDER_SCOPE_POP);
    if (camera.decoded != s_hintStats.mainCameraHints) {
        append_item(problems, fmt::format("aurora decoded {} cameras, dusk sent {}", camera.decoded,
                                  s_hintStats.mainCameraHints));
    }
    if (stage.decoded != s_hintStats.stageHints) {
        append_item(problems, fmt::format("aurora decoded {} stages, dusk sent {}", stage.decoded,
                                  s_hintStats.stageHints));
    }
    if (push.decoded != pop.decoded) {
        append_item(problems,
            fmt::format("aurora decoded {} scope pushes, {} pops", push.decoded, pop.decoded));
    }
    for (const uint16_t id : kPainterHintIds) {
        const ar::HintCounters counters = hint_delta(id);
        if (counters.malformed != 0 || counters.late != 0) {
            append_item(problems, fmt::format("{} malformed {} late {}", hint_id_name(id),
                                      counters.malformed, counters.late));
        }
    }
    return problems;
}

}  // namespace

bool hints_active() noexcept {
    return s_hintsOn;
}

void begin_painter() {
    if (s_painterOpen) {
        close_frame();
    }
    // The previous frame has drained by now, so the baselines line up with the zeroed stats
    if (s_resetRequested) {
        s_hintStats = {};
        for (size_t i = 0; i < kHintIdCount; ++i) {
            s_baseline[i] = ar::hint_counters(static_cast<uint16_t>(kFirstHintId + i));
        }
        s_resetRequested = false;
    }
    s_hintsOn = ar::hints_enabled();
    s_frame = {};
    s_pausedAtPrePost = false;
    s_painterFrame = aurora::gfx::current_frame();
    s_painterOpen = s_hintsOn;
}

void submit_main_camera(const camera_process_class& camera, const Mtx view, const Mtx44 proj) {
    if (!s_hintsOn) {
        return;
    }
    note_open_scopes(std::nullopt);
    if (!s_frame.accept_main_camera()) {
        refuse(std::nullopt);
        return;
    }
    GXAuroraSetCamera(AURORA_CAMERA_MAIN, view, proj, GX_PERSPECTIVE, kSingleCutEpoch);
    s_cutEpoch = kSingleCutEpoch;
    s_lastEye = camera.view.lookat.eye;
    s_lastFovy = camera.view.fovy;
    ++s_hintStats.mainCameraHints;
}

void mark_stage(ar::Stage stage) {
    if (!s_hintsOn) {
        return;
    }
    emit_stage(stage);
}

void end_scene() {
    if (!s_hintsOn) {
        return;
    }
    const MissingStages missing = s_frame.close_scene();
    for (uint8_t i = 0; i < missing.count; ++i) {
        emit_stage(missing.stages[i]);
    }
}

bool has_main_camera() noexcept {
    return s_hintsOn && s_frame.main_camera();
}

uint32_t current_cut_epoch() noexcept {
    return s_cutEpoch;
}

const HintStats& hint_stats() noexcept {
    return s_hintStats;
}

void reset_hint_stats() noexcept {
    s_resetRequested = true;
}

std::vector<std::string> describe_hints() {
    if (!ar::hints_enabled()) {
        return {"hints: off (AURORA_RENDER_HINTS=0 or render module not built)"};
    }
    const HintStats& stats = s_hintStats;
    std::vector<std::string> lines;
    lines.push_back(fmt::format("hints: on, frames {}, scene {} (paused {}), camera-less {}, "
                                "other {}, refused {}, open at marker {}",
        stats.frames, stats.sceneFrames, stats.pausedSceneFrames, stats.hudOnlyFrames,
        stats.otherFrames, stats.refused, stats.openAtMarker));
    lines.push_back(fmt::format("hints: main cameras {}, stages {}, last frame {}",
        stats.mainCameraHints, stats.stageHints, stage_list(stats.lastStages)));
    lines.push_back(
        fmt::format("hints: main camera eye ({:.1f}, {:.1f}, {:.1f}) fovy {:.1f}, cut epoch {}",
            s_lastEye.x, s_lastEye.y, s_lastEye.z, s_lastFovy, s_cutEpoch));

    uint64_t malformed = 0;
    uint64_t late = 0;
    for (const uint16_t id : kPainterHintIds) {
        const ar::HintCounters counters = hint_delta(id);
        malformed += counters.malformed;
        late += counters.late;
    }
    lines.push_back(fmt::format(
        "hints: aurora decoded camera {}, stage {}, scope push {} pop {}; malformed {}, late {}",
        hint_delta(GX_AURORA_RENDER_CAMERA).decoded, hint_delta(GX_AURORA_RENDER_STAGE).decoded,
        hint_delta(GX_AURORA_RENDER_SCOPE_PUSH).decoded,
        hint_delta(GX_AURORA_RENDER_SCOPE_POP).decoded, malformed, late));

    const TagStats tags = last_tag_stats();
    lines.push_back(fmt::format(
        "tags: last frame {} {}, open {}", tags.frame, tag_count_text(tags), open_tag_scopes()));
    return lines;
}

std::vector<std::string> describe_hint_ids() {
    std::vector<std::string> lines;
    ar::HintCounters total;
    for (size_t i = 0; i < kHintIdCount; ++i) {
        const auto id = static_cast<uint16_t>(kFirstHintId + i);
        const ar::HintCounters counters = hint_delta(id);
        total.decoded += counters.decoded;
        total.skipped += counters.skipped;
        total.malformed += counters.malformed;
        total.late += counters.late;
        total.bytes += counters.bytes;
        if (counters != ar::HintCounters{}) {
            lines.push_back(
                fmt::format("ids: 0x{:04X} {} {}", id, hint_id_name(id), counter_text(counters)));
        }
    }
    lines.push_back(fmt::format("ids: total {}", counter_text(total)));
    return lines;
}

void hints_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("hints.scene_frame", test_scene_frame());
    check("hints.pause_frame", test_pause_frame());
    check("hints.camera_less_frame", test_camera_less_frame());
    check("hints.refusals", test_refusals());
    check("hints.other_patterns", test_other_patterns());
    check("hints.open_at_marker", test_open_at_marker());

    const char* liveSkip = !s_hintsOn              ? "hints are off" :
                           s_hintStats.frames == 0 ? "no painter frame since the last reset" :
                                                     nullptr;
    if (liveSkip != nullptr) {
        report.skipped.push_back(fmt::format("hints.live_frames: {}", liveSkip));
        report.skipped.push_back(fmt::format("hints.aurora_agrees: {}", liveSkip));
        return;
    }
    check("hints.live_frames", test_live_frames());
    check("hints.aurora_agrees", test_aurora_agrees());
}

}  // namespace dusk::render
