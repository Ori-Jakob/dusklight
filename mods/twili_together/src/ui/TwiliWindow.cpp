#include "ui/TwiliWindow.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"

#include <mods/svc/ui.h>

#include <string>

namespace twili::ui {
namespace {

constexpr const char* kColorPresets[] = {
    "ffffff", "e53935", "fb8c00", "fdd835", "43a047", "00acc1", "1e88e5", "8e24aa",
};

UiMenuTabHandle s_menuTab = 0;
UiWindowHandle s_window = 0;
UiElementHandle s_statusText = 0;
std::string s_lastStatus;

void addControl(UiElementHandle pane, UiControlDesc& control) {
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr) != MOD_OK) {
        TwiliLog.warn("[ui] could not add control '{}'", control.label);
    }
}

void addString(UiElementHandle pane, config::Var var, const char* label, const char* help,
    int32_t maxLength) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = config::handle(var);
    control.max_length = maxLength;
    addControl(pane, control);
}

bool isIdle(ModContext*, void*) {
    return Session::instance().state() == SessionState::Disconnected;
}

bool isNotIdle(ModContext* ctx, void* data) {
    return !isIdle(ctx, data);
}

void onConnect(ModContext*, void*) {
    Session::instance().connect();
}

void onDisconnect(ModContext*, void*) {
    Session::instance().disconnect();
}

ModResult buildConnectionTab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, left, "Connection");
    addString(left, config::Var::ServerUrl, "Server URL",
        "<p>wss://host for internet play (the relay needs a real certificate).</p>"
        "<p>tcp://host:port for LAN play: unencrypted.</p>"
        "<p>ws://localhost:3000 for a relay on this computer.</p>",
        256);
    addString(left, config::Var::DisplayName, "Name", "<p>Shown to the other players.</p>", 32);
    addString(left, config::Var::RoomId, "Room",
        "<p>Players only see others in the same room. Empty is the public room.</p>", 64);
    addString(left, config::Var::TeamId, "Team",
        "<p>Progress is shared within a team.</p>", 64);

    UiControlDesc color = UI_CONTROL_DESC_INIT;
    color.kind = UI_CONTROL_COLOR;
    color.label = "Colour";
    color.help_rml = "<p>Your colour for the other players.</p>";
    color.binding = UI_BINDING_CONFIG_VAR;
    color.config_var = config::handle(config::Var::Color);
    color.color_presets = kColorPresets;
    color.color_preset_count = std::size(kColorPresets);
    addControl(left, color);

    UiControlDesc reconnect = UI_CONTROL_DESC_INIT;
    reconnect.kind = UI_CONTROL_TOGGLE;
    reconnect.label = "Reconnect automatically";
    reconnect.binding = UI_BINDING_CONFIG_VAR;
    reconnect.config_var = config::handle(config::Var::AutoReconnect);
    addControl(left, reconnect);

    svc_ui->pane_add_section(mod_ctx, left, "Status");
    s_lastStatus = Session::instance().statusText();
    svc_ui->pane_add_text(mod_ctx, left, s_lastStatus.c_str(), &s_statusText);

    UiControlDesc connect = UI_CONTROL_DESC_INIT;
    connect.kind = UI_CONTROL_BUTTON;
    connect.label = "Connect";
    connect.on_pressed = onConnect;
    connect.is_disabled = isNotIdle;
    addControl(left, connect);

    UiControlDesc disconnect = UI_CONTROL_DESC_INIT;
    disconnect.kind = UI_CONTROL_BUTTON;
    disconnect.label = "Disconnect";
    disconnect.on_pressed = onDisconnect;
    disconnect.is_disabled = isIdle;
    addControl(left, disconnect);
    return MOD_OK;
}

ModResult updateConnectionTab(ModContext*, void*, ModError*) {
    std::string status = Session::instance().statusText();
    if (s_statusText != 0 && status != s_lastStatus) {
        s_lastStatus = std::move(status);
        svc_ui->elem_set_text(mod_ctx, s_statusText, s_lastStatus.c_str());
    }
    return MOD_OK;
}

void onWindowClosed(ModContext*, UiWindowHandle, void*) {
    s_window = 0;
    s_statusText = 0;
}

void onMenuSelected(ModContext*, void*) {
    if (s_window != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Connection";
    tabs[0].build = buildConnectionTab;
    tabs[0].update = updateConnectionTab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = std::size(tabs);
    desc.on_closed = onWindowClosed;
    if (svc_ui->window_push(mod_ctx, &desc, &s_window) != MOD_OK) {
        TwiliLog.warn("[ui] could not open the Twili-Together window");
        s_window = 0;
    }
}

}  // namespace

ModResult registerMenu() {
    UiMenuTabDesc desc = UI_MENU_TAB_DESC_INIT;
    desc.label = "Twili-Together";
    desc.on_selected = onMenuSelected;
    return svc_ui->register_menu_tab(mod_ctx, &desc, &s_menuTab);
}

void shutdown() {
    s_window = 0;
    s_statusText = 0;
    s_menuTab = 0;
}

}  // namespace twili::ui
