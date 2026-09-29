#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Toasts in the host overlay; res/rcss/twili-overlay.rcss styles them by type.
namespace twili::ui {

inline constexpr const char* kToastNet = "twili-net";
inline constexpr const char* kToastStory = "twili-story";
inline constexpr const char* kToastItem = "twili-item";
inline constexpr const char* kToastWarning = "warning";

// Plain text, escaped here. durationMs 0 is the host default (5 s).
void toast(std::string_view title, std::string_view body, const char* type = kToastNet,
    uint32_t durationMs = 0);
// A plain title over a body of inline RML (text with <b> and the like).
void toastInline(std::string_view title, const std::string& bodyRml, const char* type,
    uint32_t durationMs = 0);
// Title and body are RML, each a whole element.
void toastRml(const std::string& titleRml, const std::string& bodyRml, const char* type,
    uint32_t durationMs = 0);

std::string escapeRml(std::string_view text);

}  // namespace twili::ui
