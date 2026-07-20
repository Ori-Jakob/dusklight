#pragma once

#include <mods/api.h>
#include <mods/svc/config.h>

struct HookService;

namespace lighting::hooks {

struct Options {
    const ConfigService* config = nullptr;
    ConfigVarHandle enabled = 0;
    ConfigVarHandle suppression_level = 0;
    ConfigVarHandle keep_gx_sun = 0;
};

// Hooks remain installed for the mod lifetime, but every callback reads the live cvars and
// immediately continues to vanilla when the effect or suppression is disabled.
ModResult initialize(const HookService* service, const Options& options, ModError* error);
void shutdown();

}  // namespace lighting::hooks
