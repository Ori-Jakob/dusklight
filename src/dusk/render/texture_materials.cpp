#include "dusk/render/texture_materials.hpp"

#include "dusk/main.h"
#include "dusk/render/render_bridge.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <aurora/render.hpp>
#include <aurora/texture.hpp>
#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace dusk::render {
namespace {

namespace ar = aurora::render;
namespace at = aurora::texture;

constexpr borealis::Log MaterialLog{"dusk::materials"};
constexpr const char* kShippedAsset = "materials/gz2.materials.json";
constexpr std::string_view kShippedLabel = "res/materials/gz2.materials.json";
constexpr std::string_view kUserFile = "texture_materials.user.json";
constexpr std::string_view kExportDir = "material_exports";
constexpr std::string_view kUsage =
    "Usage: render.materials [layers|reload] | check [key ...] | key <key> | export <name>";

at::MaterialManifestRegistration s_game;
at::MaterialManifestRegistration s_user;
std::string s_gameError;

bool module_off() noexcept {
    return ar::feature_support(ar::Feature::TextureMaterials).reason == ar::Reason::BuildDisabled;
}

std::string asset_path(const char* assetName) {
#ifdef DUSK_ASSET_DIR
    const char* basePath = DUSK_ASSET_DIR;
#else
    const char* basePath = SDL_GetBasePath();
#endif
    if (basePath != nullptr && basePath[0] != '\0') {
        return std::string(basePath) + "res/" + assetName;
    }
    return std::string("res/") + assetName;
}

bool read_shipped(std::string& bytes, std::string& error) {
    size_t size = 0;
    void* data = SDL_LoadFile(asset_path(kShippedAsset).c_str(), &size);
    if (data == nullptr) {
        error = SDL_GetError();
        return false;
    }
    bytes.assign(static_cast<const char*>(data), size);
    SDL_free(data);
    return true;
}

at::MaterialManifestOptions game_options() noexcept {
    return {.layer = at::MaterialLayer::Game, .label = kShippedLabel};
}

std::filesystem::path user_path() {
    return ConfigPath / kUserFile;
}

std::string_view status_name(at::MaterialManifestStatus status) noexcept {
    switch (status) {
    case at::MaterialManifestStatus::Accepted:
        return "Accepted";
    case at::MaterialManifestStatus::Rejected:
        return "Rejected";
    case at::MaterialManifestStatus::Missing:
        return "Missing";
    case at::MaterialManifestStatus::Unreadable:
        return "Unreadable";
    case at::MaterialManifestStatus::Unavailable:
        return "Unavailable";
    case at::MaterialManifestStatus::Superseded:
        return "Superseded";
    }
    return "Unknown";
}

std::string_view layer_name(at::MaterialLayer layer) noexcept {
    switch (layer) {
    case at::MaterialLayer::Game:
        return "game";
    case at::MaterialLayer::Pack:
        return "pack";
    case at::MaterialLayer::Mod:
        return "mod";
    case at::MaterialLayer::User:
        return "user";
    }
    return "unknown";
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

std::string first_issue(const at::MaterialManifestInfo& info) {
    if (info.violation) {
        return fmt::format("{} at '{}' ({})", info.violation->rule, info.violation->pointer,
            info.violation->detail);
    }
    return info.status == at::MaterialManifestStatus::Unreadable ? "the file could not be read" :
                                                                   "no issue recorded";
}

std::string layer_summary(const at::MaterialManifestRegistration& registration) {
    const auto info = at::material_manifest_info(registration);
    if (!info) {
        return fmt::format("{} 0", status_name(registration.status));
    }
    return fmt::format("{} {}", status_name(info->status), info->materialCount);
}

void warn_if_refused(std::string_view name, const at::MaterialManifestRegistration& registration) {
    const auto info = at::material_manifest_info(registration);
    if (!info || (info->status != at::MaterialManifestStatus::Rejected &&
                     info->status != at::MaterialManifestStatus::Unreadable))
    {
        return;
    }
    MaterialLog.warn("Texture materials: {} layer {} {}: {}", name, info->label,
        status_name(info->status), first_issue(*info));
}

struct Shipped {
    std::string error;
    std::vector<std::string> keys;
};

bool read_shipped_keys(Shipped& shipped) {
    std::string bytes;
    if (!read_shipped(bytes, shipped.error)) {
        return false;
    }
    const auto doc = nlohmann::json::parse(bytes, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        shipped.error = "not a JSON object";
        return false;
    }
    if (const auto materials = doc.find("materials");
        materials != doc.end() && materials->is_object())
    {
        for (const auto& item : materials->items()) {
            shipped.keys.push_back(item.key());
        }
    }
    return true;
}

std::vector<std::string> unmerged_keys(const std::vector<std::string>& keys) {
    const auto merged = nlohmann::json::parse(at::material_manifest_json(false), nullptr, false);
    const nlohmann::json* materials = nullptr;
    if (!merged.is_discarded() && merged.is_object()) {
        if (const auto it = merged.find("materials"); it != merged.end() && it->is_object()) {
            materials = &*it;
        }
    }
    std::vector<std::string> missing;
    for (const auto& key : keys) {
        if (materials == nullptr || !materials->contains(key)) {
            missing.push_back(key);
        }
    }
    return missing;
}

struct GameCheck {
    std::string problems;
    std::vector<std::string> keys;
    std::vector<std::string> unmerged;
};

GameCheck check_game_layer() {
    GameCheck result;
    if (s_game.id == 0) {
        append_problem(result.problems,
            fmt::format("game layer not registered ({}){}", status_name(s_game.status),
                s_gameError.empty() ? std::string{} : ": " + s_gameError));
    }
    Shipped shipped;
    if (!read_shipped_keys(shipped)) {
        append_problem(
            result.problems, fmt::format("cannot read {}: {}", kShippedLabel, shipped.error));
        return result;
    }
    result.keys = std::move(shipped.keys);
    if (const auto info = at::material_manifest_info(s_game)) {
        if (info->status != at::MaterialManifestStatus::Accepted) {
            append_problem(result.problems,
                fmt::format("game layer {}: {}", status_name(info->status), first_issue(*info)));
        } else if (info->materialCount != result.keys.size()) {
            append_problem(
                result.problems, fmt::format("game layer holds {} materials, the file has {}",
                                     info->materialCount, result.keys.size()));
        }
    }
    result.unmerged = unmerged_keys(result.keys);
    if (!result.unmerged.empty()) {
        append_problem(
            result.problems, fmt::format("{} shipped key(s) not merged", result.unmerged.size()));
    }
    return result;
}

void describe_layers(std::vector<std::string>& out) {
    const at::MaterialReport report = at::material_report();
    for (const auto& manifest : report.manifests) {
        std::string line = fmt::format("render.materials: {} {} {} {}", layer_name(manifest.layer),
            manifest.label, status_name(manifest.status), manifest.materialCount);
        if (manifest.layer == at::MaterialLayer::Pack || manifest.layer == at::MaterialLayer::Mod) {
            line += fmt::format(", priority {}", manifest.priority);
        }
        if (manifest.overridden) {
            line += ", session override";
        }
        if (!manifest.layerEnabled) {
            line += ", layer off";
        }
        if (manifest.violation) {
            line +=
                fmt::format(", {} at '{}'", manifest.violation->rule, manifest.violation->pointer);
        }
        out.push_back(std::move(line));
    }
    if (report.manifests.empty()) {
        out.emplace_back("render.materials: no manifests registered");
    }
    const at::MaterialUsage usage = at::material_usage();
    out.push_back(
        fmt::format("render.materials: materialCount {}, sourceKeyBuilds {}, materialLookups {}, "
                    "materialHits {}",
            usage.materialCount, usage.sourceKeyBuilds, usage.materialLookups, usage.materialHits));
}

bool check_keys(std::span<const std::string> named, std::vector<std::string>& out) {
    GameCheck game = check_game_layer();
    for (const auto& key : game.keys) {
        if (std::find(game.unmerged.begin(), game.unmerged.end(), key) != game.unmerged.end()) {
            out.push_back(fmt::format("render.materials: FAIL {} not merged", key));
        } else {
            out.push_back(fmt::format("render.materials: {} {} hits", key, at::material_hits(key)));
        }
    }
    std::string problems = std::move(game.problems);
    for (const auto& text : named) {
        const auto key = at::parse_material_key(text);
        if (!key) {
            out.push_back(fmt::format("render.materials: FAIL {} is not a material key", text));
            append_problem(problems, fmt::format("{} is not a key", text));
            continue;
        }
        const std::string canonical = at::format_material_key(*key);
        if (at::find_material_json(*key).empty()) {
            out.push_back(fmt::format("render.materials: FAIL {} not merged", canonical));
            append_problem(problems, fmt::format("{} not merged", canonical));
            continue;
        }
        const uint64_t hits = at::material_hits(canonical);
        if (hits == 0) {
            out.push_back(fmt::format("render.materials: FAIL {} 0 hits", canonical));
            append_problem(problems, fmt::format("{} 0 hits", canonical));
            continue;
        }
        out.push_back(fmt::format("render.materials: {} {} hits", canonical, hits));
    }
    if (!problems.empty()) {
        out.push_back(fmt::format("render.materials: check failed: {}", problems));
        return false;
    }
    out.push_back(fmt::format("render.materials: check passed, {} shipped and {} named key(s)",
        game.keys.size(), named.size()));
    return true;
}

bool reload_layers(std::vector<std::string>& out) {
    std::string bytes;
    std::string error;
    if (!read_shipped(bytes, error)) {
        out.push_back(
            fmt::format("render.materials: cannot read {}: {}; kept the loaded game layer",
                kShippedLabel, error));
        return false;
    }
    // Register the new text first so the game layer never drops out of a published merge
    const at::MaterialManifestRegistration previous =
        std::exchange(s_game, at::register_material_manifest(bytes, game_options()));
    s_gameError.clear();
    if (previous.id != 0) {
        at::unregister_material_manifest(previous);
    }
    at::reload_material_manifest(s_user);
    describe_layers(out);
    return true;
}

bool describe_key(std::string_view text, std::vector<std::string>& out) {
    const auto key = at::parse_material_key(text);
    if (!key) {
        out.push_back(fmt::format("render.materials: {} is not a material key", text));
        return false;
    }
    const std::string canonical = at::format_material_key(*key);
    const std::string entry = at::find_material_json(*key);
    if (entry.empty()) {
        out.push_back(fmt::format("render.materials: {} has no merged entry", canonical));
        return true;
    }
    const auto doc = nlohmann::json::parse(entry, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        out.push_back(fmt::format("render.materials: {} {}", canonical, entry));
        return true;
    }
    out.push_back(fmt::format("render.materials: {} merged as {} (id {}), {} hits", canonical,
        doc.value("key", std::string{}), doc.value("id", 0u), at::material_hits(canonical)));
    if (const auto material = doc.find("material"); material != doc.end()) {
        out.push_back(fmt::format("render.materials: material {}", material->dump()));
    }
    if (const auto origins = doc.find("origins"); origins != doc.end()) {
        out.push_back(fmt::format("render.materials: origins {}", origins->dump()));
    }
    return true;
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
        return true;
    }
    return std::filesystem::absolute(a, ec).lexically_normal() ==
           std::filesystem::absolute(b, ec).lexically_normal();
}

bool export_merge(std::string_view name, std::vector<std::string>& out) {
    if (name.size() <= 5 || !name.ends_with(".json") || name.find_first_of("/\\:") != name.npos) {
        out.emplace_back("render.materials: export takes a bare .json file name");
        return false;
    }
    std::error_code ec;
    const std::filesystem::path dir = ConfigPath / kExportDir;
    const std::filesystem::path path =
        std::filesystem::absolute(dir / borealis::io::fs_path_from_utf8(name), ec)
            .lexically_normal();
    const std::string pathString = borealis::io::fs_path_to_string(path);
    for (const auto& manifest : at::material_report().manifests) {
        if (!manifest.path.empty() && same_file(manifest.path, path)) {
            out.push_back(fmt::format("render.materials: {} is a registered manifest", pathString));
            return false;
        }
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        out.push_back(fmt::format("render.materials: cannot create {}: {}",
            borealis::io::fs_path_to_string(path.parent_path()), ec.message()));
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << at::material_manifest_json(true) << '\n';
    file.close();
    if (!file) {
        out.push_back(fmt::format("render.materials: cannot write {}", pathString));
        return false;
    }
    out.push_back(fmt::format("render.materials: wrote {}", pathString));
    return true;
}

}  // namespace

void load_texture_materials() {
    if (module_off()) {
        MaterialLog.info("Texture materials are unavailable in this build");
        return;
    }
    std::string bytes;
    if (read_shipped(bytes, s_gameError)) {
        s_game = at::register_material_manifest(bytes, game_options());
    } else {
        MaterialLog.warn("Texture materials: cannot read {}: {}", kShippedLabel, s_gameError);
    }
    s_user = at::load_material_manifest(
        user_path(), {.layer = at::MaterialLayer::User, .writable = true});
    MaterialLog.info("Texture materials: game {}, user {} ({})", layer_summary(s_game),
        layer_summary(s_user), borealis::io::fs_path_to_string(user_path()));
    warn_if_refused("game", s_game);
    warn_if_refused("user", s_user);
}

bool texture_materials_command(std::span<const std::string> args, std::vector<std::string>& out) {
    if (module_off()) {
        out.emplace_back("render.materials: unavailable in this build");
        return false;
    }
    const std::string_view verb = args.empty() ? std::string_view{"layers"} : args[0];
    if (args.size() <= 1 && verb == "layers") {
        describe_layers(out);
        return true;
    }
    if (args.size() == 1 && verb == "reload") {
        return reload_layers(out);
    }
    if (verb == "check") {
        return check_keys(args.subspan(1), out);
    }
    if (args.size() == 2 && verb == "key") {
        return describe_key(args[1], out);
    }
    if (args.size() == 2 && verb == "export") {
        return export_merge(args[1], out);
    }
    out.emplace_back(kUsage);
    return false;
}

void texture_materials_self_test(SelfTestReport& report) {
    if (module_off()) {
        report.skipped.emplace_back("materials.game_layer: unavailable in this build");
        report.skipped.emplace_back("materials.user_layer: unavailable in this build");
        return;
    }
    if (const GameCheck game = check_game_layer(); game.problems.empty()) {
        ++report.passed;
    } else {
        report.failed.push_back(fmt::format("materials.game_layer: {}", game.problems));
    }

    const auto user = at::material_manifest_info(s_user);
    if (!user) {
        report.failed.emplace_back("materials.user_layer: user manifest is not registered");
    } else if (user->layer != at::MaterialLayer::User) {
        report.failed.push_back(fmt::format(
            "materials.user_layer: registered on the {} layer", layer_name(user->layer)));
    } else if (user->status == at::MaterialManifestStatus::Accepted ||
               user->status == at::MaterialManifestStatus::Missing)
    {
        ++report.passed;
    } else if (user->status == at::MaterialManifestStatus::Rejected ||
               user->status == at::MaterialManifestStatus::Unreadable)
    {
        report.skipped.push_back(fmt::format(
            "materials.user_layer: {} {}", status_name(user->status), first_issue(*user)));
    } else {
        report.failed.push_back(
            fmt::format("materials.user_layer: status {}", status_name(user->status)));
    }
}

}  // namespace dusk::render
