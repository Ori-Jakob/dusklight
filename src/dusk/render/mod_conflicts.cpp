#include "dusk/render/mod_conflicts.hpp"

#include "dusk/render/render_bridge.hpp"
#include "dusk/ui/settings_graphics.hpp"

#include <aurora/render.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>

namespace dusk::render {
namespace {

namespace ar = aurora::render;

struct ModConflict {
    std::string_view modId;
    ar::Feature feature;
    std::string_view setting;
};

// Mods whose effect a renderer feature replaces; running both would apply it twice
constexpr std::array kModConflicts{
    ModConflict{"dev.twilitrealm.ao_mod", ar::Feature::Ao, "Ambient Occlusion"},
    ModConflict{"dev.twilitrealm.shadow_mod", ar::Feature::Shadows, "Shadow Quality"},
};

template <typename Enabled>
std::optional<std::string> find_conflict(std::string_view modId, const Enabled& enabled) {
    for (const ModConflict& conflict : kModConflicts) {
        if (conflict.modId == modId && enabled(conflict.feature)) {
            return fmt::format("Graphics > {} is on and replaces this mod. Turn it off, then "
                               "enable the mod again.",
                conflict.setting);
        }
    }
    return std::nullopt;
}

std::string test_table() {
    const auto rows = ui::graphics_rows();
    const auto features = all_features();
    for (size_t i = 0; i < kModConflicts.size(); ++i) {
        const ModConflict& conflict = kModConflicts[i];
        for (size_t j = 0; j < i; ++j) {
            if (kModConflicts[j].modId == conflict.modId ||
                kModConflicts[j].feature == conflict.feature)
            {
                return fmt::format("{} repeats a mod id or feature", conflict.modId);
            }
        }
        if (std::ranges::find(features, conflict.feature) == features.end()) {
            return fmt::format("{} names an unknown feature", conflict.modId);
        }
        const bool hasRow = std::ranges::any_of(rows, [&](const ui::GraphicsRowInfo& row) {
            return std::string_view{row.label} == conflict.setting;
        });
        if (!hasRow) {
            return fmt::format("{} names no graphics row '{}'", conflict.modId, conflict.setting);
        }
    }
    return {};
}

std::string test_rules() {
    const auto none = [](ar::Feature) { return false; };
    const auto all = [](ar::Feature) { return true; };
    const auto onlyAo = [](ar::Feature f) { return f == ar::Feature::Ao; };
    const auto onlyShadows = [](ar::Feature f) { return f == ar::Feature::Shadows; };
    if (find_conflict("dev.twilitrealm.ao_mod", none) ||
        find_conflict("dev.twilitrealm.shadow_mod", none))
    {
        return "a mod conflicts with every feature off";
    }
    const auto ao = find_conflict("dev.twilitrealm.ao_mod", onlyAo);
    if (!ao || *ao != "Graphics > Ambient Occlusion is on and replaces this mod. Turn it off, "
                      "then enable the mod again.")
    {
        return fmt::format("ao_mod with Ao on: {}", ao.value_or("no conflict"));
    }
    if (find_conflict("dev.twilitrealm.shadow_mod", onlyAo) ||
        find_conflict("dev.twilitrealm.ao_mod", onlyShadows))
    {
        return "a mod conflicts with another mod's feature";
    }
    const auto shadows = find_conflict("dev.twilitrealm.shadow_mod", onlyShadows);
    if (!shadows || shadows->find("Graphics > Shadow Quality is on") != 0) {
        return fmt::format("shadow_mod with Shadows on: {}", shadows.value_or("no conflict"));
    }
    if (find_conflict("dev.twilitrealm.window_demo", all) ||
        find_conflict("dev.twilitrealm.ao_mod2", all) || find_conflict("", all))
    {
        return "a mod outside the table conflicts";
    }
    return {};
}

}  // namespace

std::optional<std::string> mod_render_conflict(std::string_view modId) {
    const ar::Settings& settings = last_clamped();
    return find_conflict(
        modId, [&](ar::Feature feature) { return ar::feature_enabled(settings, feature); });
}

void mod_conflicts_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("conflicts.table", test_table());
    check("conflicts.rules", test_rules());
}

}  // namespace dusk::render
