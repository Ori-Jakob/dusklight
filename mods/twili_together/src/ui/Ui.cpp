#include "ui/Ui.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "ui/ItemToasts.hpp"
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
    item_toasts::clearForTest();
    s_wasConnected = false;
}

}  // namespace twili::ui
