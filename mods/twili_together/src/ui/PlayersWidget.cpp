#include "ui/PlayersWidget.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Maps.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"
#include "ui/Toasts.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"

#include <mods/svc/ui.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace twili::ui {

std::string areaText(const Client& client) {
    if (!client.isSaveLoaded || client.stageName[0] == '\0') {
        return "not in game";
    }
    const std::string stage(client.stageName, strnlen(client.stageName, sizeof(client.stageName)));
    const char* name = maps::areaName(stage.c_str(), client.roomNo);
    return name != nullptr ? name : stage;
}

namespace players_widget {
namespace {

using Clock = std::chrono::steady_clock;
using config::Var;

constexpr const char* kTeleportIdPrefix = "twili-tp-";
constexpr auto kCreateRetry = std::chrono::seconds(1);

UiElementHandle s_panel = 0;
Clock::time_point s_retryAt{};
std::string s_lastRml;
bool s_visible = true;
int s_placedX = -1;
int s_placedY = -1;
Probe s_probe;

bool hostSupports() {
    return SERVICE_HAS(svc_ui, UiService, overlay_panel_destroy) &&
           svc_ui->overlay_panel_create != nullptr &&
           svc_ui->overlay_panel_set_position != nullptr &&
           svc_ui->overlay_panel_destroy != nullptr;
}

bool menuOpen() {
    bool visible = false;
    return svc_ui->is_any_document_visible(mod_ctx, &visible) == MOD_OK && visible;
}

std::vector<Row> buildRows() {
    std::vector<Row> rows;
    const Session& session = Session::instance();
    const std::string& team = session.selfTeamId();
    for (const auto& [id, c] : session.clients()) {
        if (c.self || !c.online || (!team.empty() && c.teamId != team)) {
            continue;
        }
        Row row;
        row.clientId = id;
        row.name = teleport::clientName(id);
        row.area = areaText(c);
        row.offer = teleport::offer(id);
        if (row.offer != teleport::Offer::None) {
            row.tooltip = teleport::offerTooltip(id);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string colorCss(const Client& c) {
    return fmt::format("#{:02X}{:02X}{:02X}", c.colorR, c.colorG, c.colorB);
}

// The frame is ours, so its opacity setting leaves the text and buttons opaque.
std::string buildRml(const std::string& header, const std::vector<Row>& rows, int opacity) {
    const Session& session = Session::instance();
    std::string rml = fmt::format(
        "<div class=\"twili-pw\" style=\"background-color: rgba(21, 22, 16, {0}%); "
        "border-color: rgba(146, 135, 91, {0}%);\"><div class=\"twili-pw-head\" "
        "drag-handle=\"\"><span class=\"twili-pw-title\">{1}</span><span "
        "class=\"twili-pw-grip\">&#xe945;</span></div>",
        std::clamp(opacity, 0, 100), escapeRml(header));
    for (const Row& row : rows) {
        const Client& c = session.clients().at(row.clientId);
        rml += fmt::format("<div class=\"twili-pw-row\"><span class=\"twili-pw-dot\" "
                           "style=\"background-color: {};\"></span><span "
                           "class=\"twili-pw-name\">{}</span><span "
                           "class=\"twili-pw-area\">{}</span>",
            colorCss(c), escapeRml(row.name), escapeRml(row.area));
        if (row.offer != teleport::Offer::None) {
            rml += fmt::format("<span id=\"{}{}\" class=\"twili-pw-tp{}\">&#xe5c8;<span "
                               "class=\"twili-pw-tip\">{}</span></span>",
                kTeleportIdPrefix, row.clientId,
                row.offer == teleport::Offer::Blocked ? " blocked" : "", escapeRml(row.tooltip));
        }
        rml += "</div>";
    }
    return rml + "</div>";
}

// Why the widget stays hidden, or "".
const char* hideGate(bool interactive, const std::vector<Row>& rows) {
    if (!config::getBool(Var::PlayersWidget)) {
        return "off";
    }
    if (!Session::instance().isConnected()) {
        return "notConnected";
    }
    if (!isSaveLoaded()) {
        return "noSave";
    }
    if (rows.empty()) {
        return "empty";
    }
    // With the menu open it stays up to be moved and clicked.
    if (interactive) {
        return "";
    }
    if (localCutsceneRunning() || hideRemotePlayersForCutscene()) {
        return "cutscene";
    }
    if (dComIfGp_isPauseFlag() || dMeter2Info_getWindowStatus() != 0) {
        return "menu";
    }
    return "";
}

int perMille(float fraction) {
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * 1000.0f));
}

void onClick(ModContext*, UiElementHandle, const char* elementId, void*) {
    const std::string_view id(elementId != nullptr ? elementId : "");
    if (!id.starts_with(kTeleportIdPrefix)) {
        return;
    }
    const uint32_t clientId = static_cast<uint32_t>(
        std::strtoul(elementId + std::strlen(kTeleportIdPrefix), nullptr, 10));
    if (clientId != 0 && teleport::offer(clientId) != teleport::Offer::None) {
        TwiliLog.info("[ui] players widget: teleport to {}", teleport::clientName(clientId));
        teleport::request(clientId);
    }
}

void onMoved(ModContext*, UiElementHandle, float x, float y, void*) {
    s_placedX = perMille(x);
    s_placedY = perMille(y);
    config::setInt(Var::PlayersWidgetX, s_placedX);
    config::setInt(Var::PlayersWidgetY, s_placedY);
}

bool createPanel(const std::string& rml, int x, int y) {
    UiOverlayPanelDesc desc = UI_OVERLAY_PANEL_DESC_INIT;
    desc.rml = rml.c_str();
    desc.x = static_cast<float>(x) / 1000.0f;
    desc.y = static_cast<float>(y) / 1000.0f;
    desc.on_click = onClick;
    desc.on_moved = onMoved;
    const ModResult result = svc_ui->overlay_panel_create(mod_ctx, &desc, &s_panel);
    if (result != MOD_OK) {
        s_panel = 0;
        if (result != MOD_UNAVAILABLE) {
            TwiliLog.warn(
                "[ui] players widget: overlay panel refused ({})", static_cast<int>(result));
            s_probe.supported = false;
        }
        s_retryAt = Clock::now() + kCreateRetry;
        return false;
    }
    s_lastRml = rml;
    s_visible = true;
    s_placedX = x;
    s_placedY = y;
    return true;
}

void setVisible(bool visible) {
    if (s_panel != 0 && s_visible != visible) {
        s_visible = visible;
        svc_ui->elem_set_visible(mod_ctx, s_panel, visible);
    }
}

}  // namespace

void tick() {
    if (!s_probe.supported) {
        return;
    }
    if (!hostSupports()) {
        TwiliLog.info("[ui] players widget off: the host has no overlay panels (UiService 2.4)");
        s_probe.supported = false;
        return;
    }
    const Session& session = Session::instance();
    const bool interactive = menuOpen();
    std::vector<Row> rows = session.isConnected() ? buildRows() : std::vector<Row>{};
    s_probe.gate = hideGate(interactive, rows);
    s_probe.header = session.selfTeamId().empty() ? "Players" : "Team " + session.selfTeamId();
    s_probe.rows = std::move(rows);
    const bool want = s_probe.gate[0] == '\0';
    if (!want) {
        setVisible(false);
        s_probe.shown = false;
        return;
    }

    const int x =
        static_cast<int>(std::clamp<int64_t>(config::getInt(Var::PlayersWidgetX), 0, 1000));
    const int y =
        static_cast<int>(std::clamp<int64_t>(config::getInt(Var::PlayersWidgetY), 0, 1000));
    const int opacity =
        static_cast<int>(std::clamp<int64_t>(config::getInt(Var::PlayersWidgetOpacity), 0, 100));
    const std::string rml = buildRml(s_probe.header, s_probe.rows, opacity);
    if (s_panel == 0) {
        if (Clock::now() < s_retryAt || !createPanel(rml, x, y)) {
            s_probe.shown = false;
            return;
        }
    }
    if (rml != s_lastRml) {
        s_lastRml = rml;
        svc_ui->elem_set_rml(mod_ctx, s_panel, s_lastRml.c_str());
    }
    if (x != s_placedX || y != s_placedY) {
        s_placedX = x;
        s_placedY = y;
        svc_ui->overlay_panel_set_position(
            mod_ctx, s_panel, static_cast<float>(x) / 1000.0f, static_cast<float>(y) / 1000.0f);
    }
    setVisible(true);
    s_probe.created = true;
    s_probe.shown = true;
    s_probe.x = x;
    s_probe.y = y;
    s_probe.opacity = opacity;
}

void shutdown() {
    if (s_panel != 0 && hostSupports()) {
        svc_ui->overlay_panel_destroy(mod_ctx, s_panel);
    }
    s_panel = 0;
    s_lastRml.clear();
    s_visible = true;
    s_placedX = -1;
    s_placedY = -1;
    s_retryAt = {};
    s_probe = {};
}

const Probe& probe() {
    return s_probe;
}

void clickForTest(const std::string& elementId) {
    onClick(mod_ctx, s_panel, elementId.c_str(), nullptr);
}

void moveForTest(float x, float y) {
    // As a drag would, the host moves the panel first.
    if (s_panel != 0) {
        svc_ui->overlay_panel_set_position(mod_ctx, s_panel, x, y);
    }
    onMoved(mod_ctx, s_panel, x, y, nullptr);
}

}  // namespace players_widget
}  // namespace twili::ui
