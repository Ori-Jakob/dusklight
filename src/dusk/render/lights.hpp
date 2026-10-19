#pragma once

#include <span>
#include <string>
#include <vector>

namespace dusk::render {

struct SelfTestReport;

void submit_lights();

bool lights_command(std::span<const std::string> args, std::vector<std::string>& out);

void lights_self_test(SelfTestReport& report);

}  // namespace dusk::render
