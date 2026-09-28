#include "teleport/Teleport.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"

// No teleporting until P5 replaces this file.
namespace twili::teleport {
namespace {

std::string s_status;

}  // namespace

const char* blockCode(uint32_t clientId) {
    const Session& session = Session::instance();
    if (!session.joined()) {
        return "not-connected";
    }
    const auto it = session.clients().find(clientId);
    if (it == session.clients().end() || !it->second.online) {
        return "offline";
    }
    if (!session.roomState().teleportMode) {
        return "disabled";
    }
    return "unavailable";
}

std::string reasonText(std::string_view code) {
    static constexpr struct {
        std::string_view code;
        const char* text;
    } kTexts[] = {
        {"not-connected", "not connected"},
        {"disabled", "teleporting is off in this room"},
        {"offline", "not connected"},
        {"unavailable", "not available in this version"},
    };
    for (const auto& e : kTexts) {
        if (e.code == code) {
            return e.text;
        }
    }
    return std::string(code);
}

void request(uint32_t clientId) {
    const char* code = blockCode(clientId);
    s_status = "Teleport refused: " + reasonText(code != nullptr ? code : "unavailable") + ".";
    TwiliLog.info("[teleport] request to client {} refused ({})", clientId,
        code != nullptr ? code : "unavailable");
}

const std::string& statusMessage() {
    return s_status;
}

}  // namespace twili::teleport
