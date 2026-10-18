#include "ImGuiRenderDebugWindow.hpp"

#include "ImGuiMenuTools.hpp"

#include "dusk/render/render_bridge.hpp"
#include "dusk/render/render_settings.hpp"
#include "dusk/settings.h"

#include <aurora/render.hpp>
#include <fmt/format.h>
#include <imgui.h>

#include <string>
#include <string_view>

namespace dusk {
namespace {
// Must match aurora's window ID so this section joins aurora's window instead of opening another
constexpr const char* kRendererWindowId = "Renderer###aurora_renderer";
constexpr ImVec4 kDifferingColor{1.0f, 0.8f, 0.3f, 1.0f};
constexpr ImGuiTableFlags kTableFlags =
    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

bool s_onlyDifferences = true;

const char* YesNo(bool value) {
    return value ? "yes" : "no";
}

void ShowText(std::string_view text) {
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void Cell(std::string_view text) {
    ImGui::TableNextColumn();
    ShowText(text);
}

void DrawStatus() {
    const uint32_t latency = render::last_latch_latency();
    const std::string latencyText = latency == 0 ?
                                        std::string{"not measured"} :
                                        fmt::format("{} frame{}", latency, latency == 1 ? "" : "s");
    const bool inSync =
        render::same_settings(aurora::render::effective_settings(), render::last_clamped());
    ShowText(fmt::format("Preset: {}  Recommended: {}", render::preset_display_name(),
        render::preset_name(render::recommended_preset())));
    ShowText(fmt::format("Pushes: {}  Pending: {}  Latch latency: {}  In sync: {}",
        render::push_count(), YesNo(render::is_pending()), latencyText, YesNo(inSync)));

    if (ImGui::Button("Push Now")) {
        render::push_now();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy --cvar Arguments")) {
        ImGui::SetClipboardText(render::cvar_arguments().c_str());
    }
}

void DrawHints() {
    ImGui::SeparatorText("Hints");
    for (const auto& line : render::describe_hints()) {
        ShowText(line);
    }
    // Applied by the next painter, so pressing it mid-frame cannot split a frame's counts
    if (ImGui::Button("Reset Hint Counters")) {
        render::reset_hint_stats();
    }
}

void DrawFeatures() {
    const auto features = render::all_features();
    size_t supported = 0;
    for (const auto feature : features) {
        if (aurora::render::feature_support(feature).supported) {
            ++supported;
        }
    }
    ShowText(fmt::format("Features: {}, {} supported", features.size(), supported));

    if (!ImGui::BeginTable("Features", 5, kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Feature");
    ImGui::TableSetupColumn("Supported");
    ImGui::TableSetupColumn("Reason");
    ImGui::TableSetupColumn("Active");
    ImGui::TableSetupColumn("Ready");
    ImGui::TableHeadersRow();
    for (const auto feature : features) {
        const auto support = aurora::render::feature_support(feature);
        ImGui::TableNextRow();
        Cell(render::feature_name(feature));
        Cell(YesNo(support.supported));
        Cell(render::reason_name(support.reason));
        Cell(YesNo(aurora::render::feature_active(feature)));
        Cell(YesNo(aurora::render::feature_ready(feature)));
    }
    ImGui::EndTable();
}

void DrawSettings() {
    const auto fields = render::describe_fields();
    size_t differing = 0;
    for (const auto& field : fields) {
        if (field.differs()) {
            ++differing;
        }
    }
    ImGui::Checkbox("Only Differences", &s_onlyDifferences);
    ImGui::SameLine();
    ShowText(fmt::format("Settings: {}, {} differ", fields.size(), differing));

    if (!ImGui::BeginTable("Settings", 4, kTableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("Requested");
    ImGui::TableSetupColumn("Clamped");
    ImGui::TableSetupColumn("Effective");
    ImGui::TableHeadersRow();
    for (const auto& field : fields) {
        const bool differs = field.differs();
        if (s_onlyDifferences && !differs) {
            continue;
        }
        ImGui::TableNextRow();
        if (differs) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDifferingColor);
        }
        Cell(field.key);
        Cell(field.requested);
        Cell(field.clamped);
        Cell(field.effective);
        if (differs) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}
}  // namespace

void DrawRenderDebugWindow(bool& open) {
    if (!open || !getSettings().backend.enableAdvancedSettings.getValue()) {
        return;
    }

    aurora::render::draw_debug_window(&open);
    if (!open) {
        return;
    }

    if (!ImGui::Begin(kRendererWindowId, &open)) {
        ImGui::End();
        return;
    }
    if (ImGui::CollapsingHeader("Dusklight", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawStatus();
        ImGui::Spacing();
        DrawHints();
        ImGui::Spacing();
        DrawFeatures();
        ImGui::Spacing();
        DrawSettings();
    }
    ImGui::End();
}

void ImGuiMenuTools::ShowRenderDebugWindow() {
    DrawRenderDebugWindow(m_showRenderDebugWindow);
}
}  // namespace dusk
