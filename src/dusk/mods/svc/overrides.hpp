#pragma once

// What the overlay and texture services hold per mod, read by ModsService (game thread only).

#include "dusk/mod_loader.hpp"

#include <dolphin/gx.h>
#include <aurora/texture.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dusk::mods::svc {

struct StaticOverlayInfo {
    std::string discPath;
    std::string bundlePath;
    size_t size = 0;
};

struct RuntimeOverlayInfo {
    uint64_t handle = 0;
    std::string discPath;
    // add_file: a file of the owner's bundle
    std::string bundlePath;
    // add_buffer
    std::shared_ptr<const std::vector<u8>> buffer;
    // add_base_alias: the disc file served
    std::string aliasOf;
    int32_t baseEntryNum = -1;
    size_t size = 0;
};

// The mod's overlay/ files, in the order the overlay set is built.
std::vector<StaticOverlayInfo> overlay_static_entries(LoadedMod& mod);
// The mod's runtime overlays in registration order (later entries win).
std::vector<RuntimeOverlayInfo> overlay_runtime_entries(const LoadedMod& mod);
// Bumped whenever a runtime overlay is added or removed.
uint64_t overlay_registrations_generation();
// Overlay sets pushed to Aurora since boot (each rebuilds the FST).
uint64_t overlay_sync_count();

struct TextureOverrideInfo {
    // 0 for textures/ files
    uint64_t handle = 0;
    aurora::texture::ReplacementKey key;
    uint64_t registrationId = 0;
    int32_t priority = 0;
    // Encoded .dds/.png in the owner's bundle
    std::shared_ptr<ModBundle> bundle;
    std::string bundlePath;
    // Raw texels (register_data), kept alive by keepalive
    std::shared_ptr<const void> keepalive;
    std::span<const u8> raw;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    uint32_t gxFormat = 0;
};

// The mod's live texture replacements: textures/ files first, then runtime registrations.
std::vector<TextureOverrideInfo> texture_entries(const LoadedMod& mod);
// Bumped whenever a texture replacement is registered, re-registered or removed.
uint64_t texture_registrations_generation();

}  // namespace dusk::mods::svc
