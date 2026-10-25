#pragma once

#include <span>
#include <string>
#include <vector>

namespace dusk::render {

struct SelfTestReport;

void load_texture_materials();

bool texture_materials_command(std::span<const std::string> args, std::vector<std::string>& out);

void texture_materials_self_test(SelfTestReport& report);

}  // namespace dusk::render
