#include "graphics_tuner.hpp"

#include "button.hpp"

#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/render/render_settings.hpp"
#include "dusk/settings.h"
#include "m_Do/m_Do_audio.h"

#include <dolphin/gx/GXAurora.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>

namespace dusk::ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/tuner.rcss" />
</head>
<body>
    <tuner-root id="root">
        <graphics-tuner>
            <tuner-header>
                <tuner-title id="title" />
                <carousel-container id="carousel-container" />
            </tuner-header>
            <tuner-description id="description" />
            <tuner-divider />
            <tuner-footer id="footer" />
        </graphics-tuner>
    </tuner-root>
</body>
</rml>
)RML";

Rml::String format_internal_resolution(int value) {
    u32 width = 0;
    u32 height = 0;
    AuroraGetRenderSize(&width, &height);
    if (value <= 0) {
        return fmt::format("Auto ({}×{})", width, height);
    }
    return fmt::format("{}× ({}×{})", value, width, height);
}

Rml::String format_resampler(int value) {
    switch (static_cast<Resampler>(value)) {
    case Resampler::Bilinear:
        return "Bilinear";
    case Resampler::Area:
        return "Area";
    default:
        return "";
    }
}

Rml::String format_post_process_mode(int value) {
    switch (static_cast<BloomMode>(value)) {
    case BloomMode::Off:
        return "Off";
    case BloomMode::Classic:
        return "Classic";
    case BloomMode::Dusk:
        return "Dusklight";
    default:
        return "";
    }
}

Rml::String format_times(int value) { return fmt::format("{}×", value); }

Rml::String format_percent(int value) { return fmt::format("{}%", value); }

Rml::String format_bool(int value) { return value ? "On" : "Off"; }

template <typename T>
int read_cvar(const ConfigVar<T>& var) {
    if constexpr (std::is_same_v<T, float>) {
        return static_cast<int>(var.getValue() * 100.0f + 0.5f);
    } else {
        return static_cast<int>(var.getValue());
    }
}

template <typename T>
void write_cvar(ConfigVar<T>& var, int value) {
    if constexpr (std::is_same_v<T, float>) {
        var.setValue(static_cast<float>(value) / 100.0f);
    } else if constexpr (std::is_same_v<T, bool>) {
        var.setValue(static_cast<bool>(value));
    } else {
        var.setValue(static_cast<T>(value));
    }
}

template <auto Var, typename Min, typename Max, typename Def>
const GraphicsSetting& bind(Min min, Max max, Def def, int step, Rml::String (*label)(int),
    bool watchSize = false) {
    static const GraphicsSetting desc{
        .min = static_cast<int>(min),
        .max = static_cast<int>(max),
        .defaultValue = static_cast<int>(def),
        .step = step,
        .watchesRenderSize = watchSize,
        .read = []() -> int { return read_cvar(Var()); },
        .write = [](int value) { write_cvar(Var(), value); },
        .label = label,
        .cvarName = []() -> const char* { return Var().getName(); },
        .isModified = []() -> bool { return Var().getValue() != Var().getDefaultValue(); },
    };
    return desc;
}

template <typename T, int Scale>
struct ScaledMap {
    static int to_tuner(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            return static_cast<int>(std::lround(value * Scale));
        } else {
            static_assert(Scale == 1);
            return value;
        }
    }
    static T from_tuner(int value) {
        if constexpr (std::is_floating_point_v<T>) {
            return value / static_cast<T>(Scale);
        } else {
            return value;
        }
    }
};

// The lowest stop stands for the -1 Auto sentinel
struct CloudStyleMap {
    static int to_tuner(float value) {
        return value < 0.0f ? -5 : static_cast<int>(std::lround(value * 100.0f));
    }
    static float from_tuner(int value) { return value < 0 ? -1.0f : value / 100.0f; }
};

// The lowest stop stands for the 0 Auto sentinel
struct DistantDensityMap {
    static int to_tuner(float value) {
        return value <= 0.0f ? 20 : static_cast<int>(std::lround(value * 100.0f));
    }
    static float from_tuner(int value) { return value <= 20 ? 0.0f : value / 100.0f; }
};

using PercentMap = ScaledMap<float, 100>;
using IntegerMap = ScaledMap<int, 1>;

template <auto Var, typename Map>
const GraphicsSetting& bind_graphics(int min, int max, int step, int scale, TunerFormat format) {
    static const GraphicsSetting desc{
        .min = min,
        .max = max,
        .defaultValue = Map::to_tuner(Var().getDefaultValue()),
        .step = step,
        .read = []() -> int { return Map::to_tuner(Var().getValue()); },
        .write = [](int value) { Var().setValue(Map::from_tuner(value)); },
        .cvarName = []() -> const char* { return Var().getName(); },
        .isModified = []() -> bool {
            return Map::to_tuner(Var().getValue()) != Map::to_tuner(render::reference_value(Var()));
        },
        .scale = scale,
        .format = format,
        .reference = []() -> int { return Map::to_tuner(render::reference_value(Var())); },
    };
    return desc;
}

Rml::Element* create_stepped_carousel_root(Rml::Element* parent) {
    auto* doc = parent->GetOwnerDocument();
    auto root = doc->CreateElement("stepped-carousel");
    root->SetAttribute("tabindex", "0");
    return parent->AppendChild(std::move(root));
}

Rml::Element* create_stepped_carousel_arrow(
    Rml::Element* parent, const Rml::String& className, const Rml::String& label) {
    auto* button = append(parent, "button");
    button->SetClass("stepped-carousel-arrow", true);
    button->SetClass(className, true);
    append_text(button, label);
    return button;
}

void update_carousel_arrow_color(Rml::Element* arrow, bool dim) {
    const Rml::Colourb& color = Rml::Colourb(255, 255, 255, dim ? 128 : 255);
    arrow->SetProperty(Rml::PropertyId::Color, Rml::Property(color, Rml::Unit::COLOUR));
}

}  // namespace

const GraphicsSetting& GraphicsSetting::of(GraphicsOption option) {
    switch (option) {
    case GraphicsOption::InternalResolution:
        return bind<[]() -> auto& { return getSettings().game.internalResolutionScale; }>(
            0, 12, 0, 1, format_internal_resolution, true);
    case GraphicsOption::ShadowResolution:
        return bind<[]() -> auto& { return getSettings().game.shadowResolutionMultiplier; }>(
            1, 8, 1, 1, format_times);
    case GraphicsOption::Resampler:
        return bind<[]() -> auto& { return getSettings().game.resampler; }>(
            Resampler::Bilinear, Resampler::Area, Resampler::Bilinear, 1, format_resampler);
    case GraphicsOption::BloomMode:
        return bind<[]() -> auto& { return getSettings().game.bloomMode; }>(
            BloomMode::Off, BloomMode::Dusk, BloomMode::Classic, 1, format_post_process_mode);
    case GraphicsOption::BloomMultiplier:
        return bind<[]() -> auto& { return getSettings().game.bloomMultiplier; }>(
            0, 100, 100, 10, format_percent);
    case GraphicsOption::DepthOfFieldMode:
        return bind<[]() -> auto& { return getSettings().game.depthOfFieldMode; }>(
            DepthOfFieldMode::Off, DepthOfFieldMode::Dusk, DepthOfFieldMode::Classic, 1,
            format_post_process_mode);
    case GraphicsOption::TextureReplacements:
        return bind<[]() -> auto& { return getSettings().game.enableTextureReplacements; }>(
            0, 1, 0, 1, format_bool);
    case GraphicsOption::PbrBlend:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.legacyBlend; },
            PercentMap>(0, 100, 5, 100, TunerFormat::Percent);
    case GraphicsOption::ExposureBias:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.exposureBias; },
            ScaledMap<float, 10>>(-30, 30, 1, 10, TunerFormat::Ev);
    case GraphicsOption::LegacyShadowGain:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.legacyShadowGain; },
            PercentMap>(0, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::ShadowPagePool:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.shadowPages; },
            IntegerMap>(256, 4096, 256, 1, TunerFormat::Integer);
    case GraphicsOption::ShadowPageBudget:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.shadowPageBudget; },
            IntegerMap>(8, 256, 8, 1, TunerFormat::Integer);
    case GraphicsOption::FogScattering:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.fogScattering; },
            PercentMap>(0, 200, 10, 100, TunerFormat::Percent);
    case GraphicsOption::ShaftStrength:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.shaftStrength; },
            PercentMap>(0, 200, 10, 100, TunerFormat::Percent);
    case GraphicsOption::CloudStyle:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudStyle; },
            CloudStyleMap>(-5, 100, 5, 100, TunerFormat::PercentAutoAtMin);
    case GraphicsOption::CloudShadowStrength:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudShadowStrength; },
            PercentMap>(0, 100, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudCoverage:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudCoverage; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudDensity:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudDensity; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudAltitude:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudAltitude; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudWindSpeed:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudWindSpeed; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudSilverLining:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudSilverLining; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::CloudPowder:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.cloudPowder; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::GrassDistance:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.grassDistanceM; },
            IntegerMap>(0, 256, 8, 1, TunerFormat::MetresAutoAtMin);
    case GraphicsOption::GrassDensity:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.grassDensity; },
            PercentMap>(25, 200, 5, 100, TunerFormat::Percent);
    case GraphicsOption::GrassDistantDensity:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.grassDistantDensity; },
            DistantDensityMap>(20, 50, 5, 100, TunerFormat::PercentAutoAtMin);
    case GraphicsOption::GrassWind:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.grassWind; },
            PercentMap>(0, 200, 10, 100, TunerFormat::Percent);
    case GraphicsOption::TerrainStrength:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.terrainStrength; },
            PercentMap>(0, 200, 10, 100, TunerFormat::Percent);
    case GraphicsOption::TerrainDistance:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.terrainDistanceM; },
            IntegerMap>(0, 200, 10, 1, TunerFormat::MetresAutoAtMin);
    case GraphicsOption::TaaSharpness:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.taaSharpness; },
            PercentMap>(0, 100, 5, 100, TunerFormat::Percent);
    case GraphicsOption::DebugView:
        return bind_graphics<[]() -> auto& { return getSettings().graphics.debugView; },
            IntegerMap>(0, 63, 1, 1, TunerFormat::ViewAutoAtMin);
    }
    DuskLog.error("{} is an invalid GraphicsOption", static_cast<int>(option));
    abort();
}

Rml::String GraphicsSetting::format_value(int value) const {
    if (label != nullptr) {
        return label(value);
    }
    switch (format) {
    case TunerFormat::Percent:
        return format_percent(value);
    case TunerFormat::PercentAutoAtMin:
        return value == min ? "Auto" : format_percent(value);
    case TunerFormat::MetresAutoAtMin:
        return value == min ? "Auto" : fmt::format("{} m", value);
    case TunerFormat::Ev:
        return fmt::format("{:+.1f} EV", value / static_cast<double>(scale));
    case TunerFormat::ViewAutoAtMin:
        return value == min ? "Off" : fmt::format("View {}", value);
    case TunerFormat::Integer:
    case TunerFormat::Custom:
        break;
    }
    return fmt::format("{}", value);
}

SteppedCarousel::SteppedCarousel(Rml::Element* parent, Props props)
    : Component(create_stepped_carousel_root(parent)), mProps(std::move(props)) {
    mPrevElem = create_stepped_carousel_arrow(mRoot, "prev", "\uE5CB");
    mValueElem = append(mRoot, "stepped-carousel-value");
    mNextElem = create_stepped_carousel_arrow(mRoot, "next", "\uE5CC");

    listen(mPrevElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Left); });
    listen(mNextElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Right); });
    listen(mRoot, Rml::EventId::Keydown, [this](Rml::Event& event) {
        const auto cmd = map_nav_event(event);
        if (cmd != NavCommand::None && handle_nav_command(cmd)) {
            event.StopPropagation();
        }
    });
}

bool SteppedCarousel::focus() {
    return Component::focus();
}

void SteppedCarousel::update() {}

void SteppedCarousel::refresh() {
    if (mValueElem == nullptr) {
        return;
    }
    const int value = std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (mProps.formatValue) {
        set_text_content(mValueElem, mProps.formatValue(value));
    } else {
        set_text_content(mValueElem, std::to_string(value));
    }

    update_carousel_arrow_color(mPrevElem, value == mProps.min);
    update_carousel_arrow_color(mNextElem, value == mProps.max);
}

bool SteppedCarousel::handle_nav_command(NavCommand cmd) {
    if (cmd == NavCommand::Left) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value - mProps.step, mProps.min, mProps.max));
        return true;
    }
    if (cmd == NavCommand::Right) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value + mProps.step, mProps.min, mProps.max));
        return true;
    }
    return false;
}

void SteppedCarousel::apply(int value) {
    const int nextValue = std::clamp(value, mProps.min, mProps.max);
    const int currentValue =
        std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (nextValue == currentValue) {
        return;
    }
    mDoAud_seStartMenu(kSoundItemChange);
    if (mProps.onChange) {
        mProps.onChange(nextValue);
    }
}

GraphicsTuner::GraphicsTuner(GraphicsTunerProps props)
    : Document(kDocumentSource, false, DocumentScope::GraphicsTuner),
      mSetting(GraphicsSetting::of(props.option)) {
    if (mDocument == nullptr) {
        return;
    }

    if (auto* title = mDocument->GetElementById("title")) {
        append_text(title, props.title);
    }
    if (auto* description = mDocument->GetElementById("description")) {
        append_text(description, props.helpText);
    }
    if (auto* carouselParent = mDocument->GetElementById("carousel-container")) {
        mCarousel = &add_component<SteppedCarousel>(carouselParent,
            SteppedCarousel::Props{
                .min = mSetting.min,
                .max = mSetting.max,
                .step = mSetting.step,
                .getValue = [this] { return mSetting.read(); },
                .onChange = [this](int value) { mSetting.set(value); },
                .formatValue = [this](int value) { return mSetting.format_value(value); },
            });
    }

    if (auto* footer = mDocument->GetElementById("footer")) {
        auto& returnButton = add_component<Button>(footer, "\xE2\x86\x90 Return", "footer-button")
                                 .on_pressed([this] { pop(); });
        returnButton.root()->SetClass("return", true);
        const Rml::String resetLabel =
            mSetting.reference != nullptr ? "Reset to preset" : "Reset to default";
        auto& resetButton =
            add_component<Button>(footer, resetLabel, "footer-button").on_pressed([this] {
                mDoAud_seStartMenu(kSoundItemChange);
                reset_default();
            });
        resetButton.root()->SetClass("reset", true);
    }

    if (mCarousel != nullptr) {
        if (const char* name = mSetting.cvarName()) {
            mSubscription = config::subscribe(name,
                [this](config::ConfigVarBase&, const void*) { mCarousel->refresh(); });
        }
        mCarousel->refresh();
        if (mSetting.watchesRenderSize) {
            AuroraGetRenderSize(&mLastRenderWidth, &mLastRenderHeight);
        }
    }

    // Hide document after transition completion
    mRoot = mDocument->GetElementById("root");
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });
}

GraphicsTuner::~GraphicsTuner() {
    if (mSubscription != 0) {
        config::unsubscribe(mSubscription);
    }
}

void GraphicsTuner::show() {
    Document::show();
    mRoot->SetAttribute("open", "");
    mDoAud_seStartMenu(kSoundWindowOpen);
}

void GraphicsTuner::hide(bool close) {
    config::save();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
        mDoAud_seStartMenu(kSoundWindowClose);
    }
}

void GraphicsTuner::update() {
    if (mSetting.watchesRenderSize && mCarousel != nullptr) {
        u32 width = 0;
        u32 height = 0;
        AuroraGetRenderSize(&width, &height);
        if (width != mLastRenderWidth || height != mLastRenderHeight) {
            mLastRenderWidth = width;
            mLastRenderHeight = height;
            mCarousel->refresh();
        }
    }
    for (const auto& component : mComponents) {
        component->update();
    }
    Document::update();
}

bool GraphicsTuner::focus() {
    for (const auto& component : mComponents) {
        if (component->focus()) {
            return true;
        }
    }
    return false;
}

bool GraphicsTuner::visible() const {
    return mRoot->HasAttribute("open");
}

bool GraphicsTuner::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel) {
        pop();
        return true;
    }

    if (mCarousel && mCarousel->handle_nav_command(cmd)) {
        return true;
    }

    return Document::handle_nav_command(event, cmd);
}

void GraphicsTuner::reset_default() {
    mSetting.set(mSetting.referenceValue());
}

}  // namespace dusk::ui
