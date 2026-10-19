#pragma once

#include <span>
#include <string>
#include <vector>

namespace dusk::render {

struct SelfTestReport;

void submit_lights();
void submit_environment();
void fog_probe_close() noexcept;

bool sky_visible();

bool lights_command(std::span<const std::string> args, std::vector<std::string>& out);
bool environment_command(std::span<const std::string> args, std::vector<std::string>& out);

void lights_self_test(SelfTestReport& report);
void environment_self_test(SelfTestReport& report);

}  // namespace dusk::render
