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

constexpr std::array<std::string_view, 1> kFsrFiles{"fidelityfx-fsr/LICENSE.txt"};
constexpr std::array<std::string_view, 1> kDawnFiles{"dawn/LICENSE.txt"};
constexpr std::array<std::string_view, 1> kSdlFiles{"sdl3/LICENSE.txt"};
constexpr std::array<std::string_view, 2> kDxcFiles{"dxc/LICENSE.txt", "dxc/dxil-LICENSE.txt"};
constexpr std::array<std::string_view, 2> kNodFiles{
    "nod/LICENSE-MIT.txt", "nod/LICENSE-APACHE.txt"};
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
        "The FSR upscaler is a port of FidelityFX Super Resolution 3.1.5.", kFsrFiles},
    LicenseComponent{
        "Dawn", "BSD 3-Clause", "The WebGPU implementation that draws the game.", kDawnFiles},
    LicenseComponent{"SDL", "zlib", "Windows, input, audio and file access.", kSdlFiles},
    LicenseComponent{"DirectX Shader Compiler", "NCSA, Microsoft",
        "Compiles shaders for Direct3D 12 (dxcompiler.dll and dxil.dll).", kDxcFiles},
    LicenseComponent{"nod", "MIT or Apache 2.0", "Reads the game disc image.", kNodFiles},
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
