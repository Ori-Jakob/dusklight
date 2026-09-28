#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace twili::autotest {

struct StepContext {
    const nlohmann::json& step;
    // False on the step's first call.
    bool begun;
    int ticks;
    double seconds;
    double timeout(double fallback) const { return step.value("timeoutSec", fallback); }
    void fail(const std::string& reason) const;
};

// Returns true when done, false to be called again next tick, nullopt for an unknown op.
using StepHandler = std::optional<bool> (*)(const std::string& op, StepContext& ctx);

// For feature step files: static const bool sRegistered = registerSteps(&handler);
bool registerSteps(StepHandler handler);

}  // namespace twili::autotest
