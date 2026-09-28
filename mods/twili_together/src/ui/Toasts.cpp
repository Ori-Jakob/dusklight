#include "ui/Toasts.hpp"

#include "core/Log.hpp"

#include <mods/svc/ui.h>

namespace twili::ui {

std::string escapeRml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c; break;
        }
    }
    return out;
}

void toastRml(const std::string& titleRml, const std::string& bodyRml, const char* type,
    uint32_t durationMs) {
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.type = type;
    desc.title_rml = titleRml.c_str();
    desc.body_rml = bodyRml.c_str();
    desc.duration_ms = durationMs;
    if (svc_ui->push_toast(mod_ctx, &desc) != MOD_OK) {
        TwiliLog.warn("[ui] toast refused: {}", titleRml);
    }
}

void toast(std::string_view title, std::string_view body, const char* type,
    uint32_t durationMs) {
    // The host inserts text starting with '<' as RML, anything else as text: always send RML,
    // in the elements the host uses for plain toasts.
    const std::string titleRml =
        title.empty() ? std::string{} : "<toast-title>" + escapeRml(title) + "</toast-title>";
    const std::string bodyRml = body.empty() ? std::string{}
                                             : "<toast-message-text>" + escapeRml(body) +
                                                   "</toast-message-text>";
    toastRml(titleRml, bodyRml, type, durationMs);
}

}  // namespace twili::ui
