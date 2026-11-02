#pragma once

#include <span>
#include <string_view>

namespace dusk::render {
struct SelfTestReport;
}

namespace dusk::ui {

class Pane;

struct LicenseComponent {
    std::string_view name;
    std::string_view license;
    std::string_view usage;
    std::span<const std::string_view> files;
};

std::span<const LicenseComponent> license_components() noexcept;
bool dlss_runtime_shipped() noexcept;
void build_licenses_tab(Pane& leftPane, Pane& rightPane);
void licenses_self_test(render::SelfTestReport& report);

}  // namespace dusk::ui
