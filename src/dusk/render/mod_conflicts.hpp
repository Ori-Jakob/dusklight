#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dusk::render {

struct SelfTestReport;

std::optional<std::string> mod_render_conflict(std::string_view modId);
void mod_conflicts_self_test(SelfTestReport& report);

}  // namespace dusk::render
