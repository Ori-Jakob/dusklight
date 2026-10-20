#pragma once

#include "dusk/mod_loader.hpp"

namespace dusk::mods {

struct LoadedManifest {
    ModMetadata metadata;
    std::optional<DelegatedModRuntime> runtime;
    // mod.json as read from the bundle, keys the loader does not parse included.
    std::string rawJson;
};

LoadedManifest load_manifest(const std::filesystem::path& modPath, ModBundle& bundle);

}  // namespace dusk::mods
