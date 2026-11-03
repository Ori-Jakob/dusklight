#include "licenses.hpp"

#include "pane.hpp"
#include "ui.hpp"

#include "dusk/render/render_bridge.hpp"

#include <fmt/format.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace dusk::ui {
namespace {

constexpr std::array<std::string_view, 2> kFsrFiles{
    "fidelityfx-fsr/LICENSE.txt", "fidelityfx-fsr/RCAS-LICENSE.txt"};
constexpr std::array<std::string_view, 5> kDawnFiles{
    "dawn/LICENSE.txt",
    "dawn/SPIRV-Tools-LICENSE.txt",
    "dawn/SPIRV-Headers-LICENSE.txt",
    "dawn/Vulkan-Headers-LICENSE.txt",
    "dawn/Vulkan-Utility-Libraries-LICENSE.txt",
};
constexpr std::array<std::string_view, 3> kSdlFiles{
    "sdl3/LICENSE.txt", "sdl3/hidapi-LICENSE-bsd.txt", "sdl3/yuv2rgb-LICENSE.txt"};
// A page draws all of its text every frame, so the largest texts get entries of their own
constexpr std::array<std::string_view, 2> kDxcFiles{"dxc/LICENSE.txt", "dxc/ThirdPartyNotices.txt"};
constexpr std::array<std::string_view, 1> kDxilFiles{"dxc/dxil-LICENSE.txt"};
constexpr std::array<std::string_view, 3> kNodFiles{
    "nod/LICENSE-MIT.txt", "nod/LICENSE-APACHE.txt", "nod/dependencies.txt"};
constexpr std::array<std::string_view, 6> kAuroraFiles{
    "aurora/LICENSE.txt",
    "aurora/hextile-demo-LICENSE.txt",
    "aurora/bevy-taa-LICENSE.txt",
    "aurora/catmull-rom-LICENSE.txt",
    "aurora/openpbr-LICENSE.txt",
    "aurora/xegtao-LICENSE.txt",
};
constexpr std::array<std::string_view, 1> kBorealisFiles{"borealis/LICENSE.txt"};
constexpr std::array<std::string_view, 1> kAbseilFiles{"abseil/LICENSE.txt"};
constexpr std::array<std::string_view, 2> kRmlUiFiles{
    "rmlui/LICENSE.txt", "rmlui/Containers-LICENSE.txt"};
constexpr std::array<std::string_view, 2> kImGuiFiles{
    "imgui/LICENSE.txt", "imgui/ProggyClean-LICENSE.txt"};
constexpr std::array<std::string_view, 4> kFreetypeFiles{
    "freetype/LICENSE.txt",
    "freetype/FTL.txt",
    "freetype/bdf-README.txt",
    "freetype/pcf-README.txt",
};
constexpr std::array<std::string_view, 1> kFmtFiles{"fmt/LICENSE.txt"};
constexpr std::array<std::string_view, 1> kPngFiles{"libpng/LICENSE.txt"};
constexpr std::array<std::string_view, 2> kZlibFiles{
    "zlib/LICENSE.txt", "zlib/zlib-ng-LICENSE.txt"};
constexpr std::array<std::string_view, 1> kZstdFiles{"zstd/LICENSE.txt"};
constexpr std::array<std::string_view, 2> kBzip2BrotliFiles{
    "bzip2-brotli/bzip2-LICENSE.txt", "bzip2-brotli/brotli-LICENSE.txt"};
constexpr std::array<std::string_view, 7> kUtilityFiles{
    "utilities/cxxopts-LICENSE.txt",
    "utilities/magic_enum-LICENSE.txt",
    "utilities/miniz-LICENSE.txt",
    "utilities/nlohmann-json-LICENSE.txt",
    "utilities/PicoSHA2-LICENSE.txt",
    "utilities/xxHash-LICENSE.txt",
    "utilities/yyjson-LICENSE.txt",
};
constexpr std::array<std::string_view, 2> kFunchookFiles{
    "funchook/LICENSE.txt", "funchook/diStorm-LICENSE.txt"};
constexpr std::array<std::string_view, 7> kSentryFiles{
    "sentry/LICENSE.txt",
    "sentry/jsmn-LICENSE.txt",
    "sentry/Crashpad-LICENSE.txt",
    "sentry/mini_chromium-LICENSE.txt",
    "sentry/mpack-LICENSE.txt",
    "sentry/zlib-LICENSE.txt",
    "sentry/getopt-LICENSE.txt",
};
constexpr std::array<std::string_view, 5> kModFiles{
    "mods/Luau-LICENSE.txt",
    "mods/LuaBridge3-LICENSE.txt",
    "mods/yaml-cpp-LICENSE.txt",
    "mods/base64pp-LICENSE.txt",
    "mods/battery-embed-LICENSE.txt",
};
constexpr std::array<std::string_view, 4> kPublicDomainFiles{
    "public-domain/SQLite.txt",
    "public-domain/Freeverb.txt",
    "public-domain/bcdec.txt",
    "public-domain/bc7enc.txt",
};
constexpr std::array<std::string_view, 6> kFontFiles{
    "fonts/AlegreyaSC-OFL.txt",
    "fonts/FiraSans-OFL.txt",
    "fonts/FiraSansCondensed-OFL.txt",
    "fonts/Inter-OFL.txt",
    "fonts/NotoMono-OFL.txt",
    "fonts/MaterialSymbolsRounded-LICENSE.txt",
};
#ifdef DUSK_SHIP_XESS
constexpr std::array<std::string_view, 2> kXessFiles{
    "xess/LICENSE.txt", "xess/third-party-programs.txt"};
#endif

constexpr std::array kComponents{
    LicenseComponent{"AMD FidelityFX Super Resolution", "MIT",
        "Temporal upscaling and sharpening of the 3D scene.", kFsrFiles},
    LicenseComponent{"Dawn", "BSD 3-Clause, Apache 2.0, MIT",
        "The WebGPU implementation that draws the game.", kDawnFiles},
    LicenseComponent{
        "SDL", "zlib, BSD 3-Clause", "Windows, input, audio and file access.", kSdlFiles},
    LicenseComponent{"DirectX Shader Compiler", "NCSA",
        "Compiles shaders for Direct3D 12 (dxcompiler.dll).", kDxcFiles},
    LicenseComponent{"DXIL Validator", "Windows SDK terms",
        "Validates and signs Direct3D 12 shaders (dxil.dll, from the Windows SDK).", kDxilFiles},
    LicenseComponent{"nod", "MIT or Apache 2.0", "Reads the game disc image.", kNodFiles},
    LicenseComponent{
        "aurora", "MIT, Apache 2.0", "The engine layer that runs the game on PC.", kAuroraFiles},
    LicenseComponent{
        "borealis", "MIT", "Logging, updates, crash handling and networking.", kBorealisFiles},
    LicenseComponent{
        "Abseil", "Apache 2.0", "C++ containers used by the engine and Dawn.", kAbseilFiles},
    LicenseComponent{"RmlUi", "MIT", "Draws the menus and settings screens.", kRmlUiFiles},
    LicenseComponent{"Dear ImGui", "MIT", "Draws the debug and developer windows.", kImGuiFiles},
    LicenseComponent{
        "FreeType", "FreeType License", "Renders the interface fonts.", kFreetypeFiles},
    LicenseComponent{"fmt", "MIT", "Formats text and log messages.", kFmtFiles},
    LicenseComponent{"libpng", "libpng", "Reads and writes PNG images.", kPngFiles},
    LicenseComponent{"zlib", "zlib", "Compression for PNG images, fonts and archives.", kZlibFiles},
    LicenseComponent{
        "Zstandard", "BSD 3-Clause", "Compresses the shader cache and mod data.", kZstdFiles},
    LicenseComponent{
        "bzip2 and Brotli", "bzip2, MIT", "Decompress compressed font files.", kBzip2BrotliFiles},
    LicenseComponent{"Utility libraries", "MIT, BSD 2-Clause",
        "cxxopts, magic_enum, miniz, nlohmann/json, PicoSHA2, xxHash and yyjson.", kUtilityFiles},
    LicenseComponent{"funchook and diStorm", "GPL 2.0+, BSD 3-Clause",
        "Installs the function hooks that code mods use.", kFunchookFiles},
    LicenseComponent{"Sentry and Crashpad", "MIT, Apache 2.0, BSD",
        "Crash reporting in official builds (crashpad_handler.exe).", kSentryFiles},
    LicenseComponent{"Mod libraries", "MIT, Apache 2.0",
        "Luau, LuaBridge3, yaml-cpp, base64pp and battery::embed, used by the bundled mods.",
        kModFiles},
    LicenseComponent{"Public domain code", "Public domain", "SQLite, Freeverb, bcdec and bc7enc.",
        kPublicDomainFiles},
    LicenseComponent{"Fonts", "OFL 1.1, Apache 2.0",
        "Alegreya SC, Fira Sans, Fira Sans Condensed, Inter, Noto Mono and Material Symbols "
        "Rounded.",
        kFontFiles},
#ifdef DUSK_SHIP_XESS
    LicenseComponent{"XeSS-SR", "Intel Simplified Software License",
        "The XeSS-SR upscaler runtime (libxess.dll), shipped unmodified in binary form.",
        kXessFiles},
#endif
};

std::filesystem::path license_path(std::string_view file) {
    return resource_path("licenses") / std::filesystem::path{file};
}

std::string read_license(std::string_view file) {
    std::ifstream in(license_path(file), std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Licence files wrap at 80 columns, so lines are joined and only blank lines break paragraphs
void add_license_text(Pane& pane, const std::string& text) {
    std::string paragraph;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string_view line{text.data() + start, end - start};
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            if (!paragraph.empty()) {
                pane.add_text(paragraph);
                paragraph.clear();
            }
        } else {
            if (!paragraph.empty()) {
                paragraph += ' ';
            }
            paragraph += line;
        }
        start = end + 1;
    }
    if (!paragraph.empty()) {
        pane.add_text(paragraph);
    }
}

void build_component(Pane& pane, const LicenseComponent& component) {
    pane.clear();
    pane.add_text(Rml::String{component.usage});
    for (const auto file : component.files) {
        if (component.files.size() > 1) {
            pane.add_section(Rml::String{std::filesystem::path{file}.filename().string()});
        }
        const std::string text = read_license(file);
        if (text.empty()) {
            pane.add_text(fmt::format("res/licenses/{} is missing.", file));
            continue;
        }
        add_license_text(pane, text);
    }
}

}  // namespace

std::span<const LicenseComponent> license_components() noexcept {
    return kComponents;
}

bool dlss_runtime_shipped() noexcept {
#ifdef DUSK_SHIP_DLSS
    return true;
#else
    return false;
#endif
}

void build_licenses_tab(Pane& leftPane, Pane& rightPane) {
    leftPane.add_section("Third-Party Software");
    for (const auto& component : license_components()) {
        leftPane.register_control(
            leftPane.add_select_button({
                .key = Rml::String{component.name},
                .getValue = [&component] { return Rml::String{component.license}; },
            }),
            rightPane, [&component](Pane& pane) { build_component(pane, component); });
    }
    rightPane.add_text("Dusklight includes the software listed here. Select an entry to read its "
                       "licence.");
}

void licenses_self_test(render::SelfTestReport& report) {
    std::string problems;
    for (const auto& component : license_components()) {
        for (const auto file : component.files) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(license_path(file), ec);
            if (ec || size == 0) {
                if (!problems.empty()) {
                    problems += ", ";
                }
                problems += fmt::format("{} is {}", file, ec ? "missing" : "empty");
            }
        }
    }
    if (problems.empty()) {
        ++report.passed;
    } else {
        report.failed.push_back("licenses.present: " + problems);
    }
}

}  // namespace dusk::ui
