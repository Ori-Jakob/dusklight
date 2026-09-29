#include "ui/Ui.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "hooks/Hooks.hpp"
#include "ui/HitMarkers.hpp"
#include "ui/ItemToasts.hpp"
#include "ui/MapCursors.hpp"
#include "ui/NameTags.hpp"
#include "ui/TwiliWindow.hpp"

#include <mods/svc/ui.h>

namespace twili::ui {
namespace {

bool s_wasConnected = false;

void registerStyles(UiStyleScope scope, const char* path) {
    UiStyleHandle handle = 0;
    if (svc_ui->register_styles_file(mod_ctx, scope, path, &handle) != MOD_OK) {
        TwiliLog.warn("[ui] could not load {}", path);
    }
}

}  // namespace

void init() {
    registerStyles(UI_SCOPE_WINDOW, "rcss/twili.rcss");
    registerStyles(UI_SCOPE_OVERLAY, "rcss/twili-overlay.rcss");
    if (registerWindow() != MOD_OK) {
        TwiliLog.warn("[ui] could not add the menu tab");
    }
    name_tags::install();
    hit_markers::install();
    std::string error;
    if (hooks::install(hooks::Group::Map, error) != MOD_OK) {
        TwiliLog.warn("[ui] map cursors are off: {}", error);
    }
}

void tick() {
    // Pending toasts belong to the connection they came from.
    const bool connected = Session::instance().isConnected();
    if (s_wasConnected && !connected) {
        item_toasts::resetSession();
    }
    s_wasConnected = connected;
    item_toasts::tick();
}

void shutdown() {
    shutdownWindow();
    name_tags::uninstall();
    hit_markers::uninstall();
    map_cursor::reset();
    item_toasts::clearForTest();
    s_wasConnected = false;
}

}  // namespace twili::ui
