// Steps for the players widget and the Players tab rows; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"
#include "autotest/Input.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "teleport/Teleport.hpp"
#include "ui/PlayersWidget.hpp"
#include "ui/TwiliWindow.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace widget = ui::players_widget;

int sHoldSince = -1;
uint32_t sTeleportSeq = 0;
int sClickStart = 0;
json sDrag;

struct Measure {
    int start = 0;
    int origX = 0;
    int origY = 0;
    uint32_t moves = 0;
};
Measure sMeasure;
// Measured widths and heights in dp, by the step's `as` name.
std::map<std::string, std::pair<float, float>> sSizes;

const char* offerName(teleport::Offer offer) {
    switch (offer) {
    case teleport::Offer::Ready:
        return "ready";
    case teleport::Offer::Blocked:
        return "blocked";
    default:
        return "none";
    }
}

std::string clientName(uint32_t id) {
    const auto& clients = Session::instance().clients();
    const auto it = clients.find(id);
    return it != clients.end() ? it->second.name : fmt::format("#{}", id);
}

uint32_t peerId(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.name == name) {
            return id;
        }
    }
    return 0;
}

bool held(bool ok, const StepContext& ctx, int holdTicks) {
    if (!ctx.begun) {
        sHoldSince = -1;
    }
    if (!ok) {
        sHoldSince = -1;
        return false;
    }
    if (sHoldSince < 0) {
        sHoldSince = ctx.ticks;
    }
    return ctx.ticks - sHoldSince >= holdTicks;
}

std::string describeWidget() {
    const widget::Probe& p = widget::probe();
    std::string s = fmt::format(
        "supported={} shown={} gate='{}' at {},{} opacity={} scale={} inGame={} header='{}' rows=[",
        p.supported, p.shown, p.gate, p.x, p.y, p.opacity, p.scale, p.clickableInGame, p.header);
    for (const widget::Row& r : p.rows) {
        s += fmt::format(" '{}' '{}' {} '{}';", r.name, r.area, offerName(r.offer), r.tooltip);
    }
    return s + " ]";
}

std::string describeTab() {
    std::string s = fmt::format("tab={} rows=[", ui::playersTabShown());
    for (const ui::PlayersTabRow& r : ui::playersTabRows()) {
        s += fmt::format(" '{}' teleport={}{} '{}' actions={};", clientName(r.clientId), r.teleport,
            r.blocked ? " blocked" : "", r.tooltip, r.actions);
    }
    return s + " ]";
}

bool contains(const std::string& text, const json& step, const char* key) {
    return !step.contains(key) || text.find(step.value(key, std::string{})) != std::string::npos;
}

// Every row in step["rows"] listed with those fields, none of step["absent"], step["count"] rows.
template <typename Row, typename Match>
bool rowsMatch(const json& step, const std::vector<Row>& rows, Match&& match) {
    if (step.contains("count") && rows.size() != step.value("count", 0u)) {
        return false;
    }
    for (const json& want : step.value("rows", json::array())) {
        const uint32_t id = peerId(want.value("name", std::string{}));
        const auto it =
            std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.clientId == id; });
        if (id == 0 || it == rows.end() || !match(want, *it)) {
            return false;
        }
    }
    for (const json& name : step.value("absent", json::array())) {
        const uint32_t id = peerId(name.get<std::string>());
        if (std::any_of(rows.begin(), rows.end(), [&](const Row& r) { return r.clientId == id; })) {
            return false;
        }
    }
    return true;
}

std::optional<bool> expectPlayersWidget(StepContext& ctx) {
    const json& step = ctx.step;
    const widget::Probe& p = widget::probe();
    const bool wantShown = step.value("shown", true);
    const auto intIs = [&](const char* key, int value, bool atLeast) {
        return !step.contains(key) ||
               (atLeast ? value >= step.value(key, 0) : value == step.value(key, 0));
    };
    bool ok = p.shown == wantShown && intIs("x", p.x, false) && intIs("y", p.y, false) &&
              intIs("minX", p.x, true) && intIs("minY", p.y, true) &&
              intIs("opacity", p.opacity, false) && intIs("scale", p.scale, false) &&
              (!step.contains("clickableInGame") ||
                  step.value("clickableInGame", false) == p.clickableInGame) &&
              (!step.contains("header") || step.value("header", std::string{}) == p.header) &&
              (wantShown || !step.contains("gate") || step.value("gate", std::string{}) == p.gate);
    if (ok && wantShown) {
        ok = rowsMatch(step, p.rows, [](const json& want, const widget::Row& r) {
            return contains(r.area, want, "area") && contains(r.tooltip, want, "tooltip") &&
                   (!want.contains("label") || want.value("label", std::string{}) == r.name) &&
                   (!want.contains("teleport") ||
                       want.value("teleport", std::string{}) == offerName(r.offer));
        });
    }
    if (held(ok, ctx, step.value("holdTicks", 0))) {
        TwiliLog.info("[autotest] players widget ok: {}", describeWidget());
        return true;
    }
    if (ctx.seconds > ctx.timeout(15.0)) {
        ctx.fail(fmt::format("expectPlayersWidget {}: {}", step.dump(), describeWidget()));
    }
    return false;
}

std::optional<bool> expectPlayersTab(StepContext& ctx) {
    const json& step = ctx.step;
    const std::vector<ui::PlayersTabRow> rows = ui::playersTabRows();
    const bool ok =
        ui::playersTabShown() &&
        rowsMatch(step, rows, [](const json& want, const ui::PlayersTabRow& r) {
            const std::string teleport = !r.teleport ? "none" : r.blocked ? "blocked" : "ready";
            return contains(r.tooltip, want, "tooltip") &&
                   (!want.contains("teleport") ||
                       want.value("teleport", std::string{}) == teleport) &&
                   (!want.contains("actions") || want.value("actions", false) == r.actions);
        });
    if (held(ok, ctx, step.value("holdTicks", 0))) {
        TwiliLog.info("[autotest] players tab ok: {}", describeTab());
        return true;
    }
    if (ctx.seconds > ctx.timeout(15.0)) {
        ctx.fail(fmt::format("expectPlayersTab {}: {}", step.dump(), describeTab()));
    }
    return false;
}

bool resultMatches(const json& step, const std::string& got) {
    const json expect = step.value("expect", json("arrived"));
    const auto one = [&](const json& e) {
        const std::string want = e.is_string() ? e.get<std::string>() : std::string{};
        return want == got || (want == "arrived" && (got == "local" || got == "stage"));
    };
    return expect.is_array() ? std::any_of(expect.begin(), expect.end(), one) : one(expect);
}

// The host's on_click for the target's teleport button, then the teleport's result.
std::optional<bool> clickPlayersWidget(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string target = step.value("target", std::string{});
    if (!ctx.begun) {
        const uint32_t id = peerId(target);
        if (id == 0) {
            ctx.fail("clickPlayersWidget: no peer named '" + target + "'");
            return false;
        }
        sTeleportSeq = teleport::status().resultSeq;
        sClickStart = ctx.ticks;
        if (!step.contains("at")) {
            widget::clickForTest(fmt::format("twili-tp-{}", id));
        }
    }
    // `at`: a real click there, through SDL's queue and the host's panel.
    if (const int t = ctx.ticks - sClickStart; step.contains("at") && t <= 2) {
        const int x = step["at"][0].get<int>();
        const int y = step["at"][1].get<int>();
        const bool posted =
            t == 0 ? input::mouseMove(x, y, false) : input::mouseButton(x, y, t == 1);
        if (!posted) {
            ctx.fail("clickPlayersWidget: no game window");
        }
        return false;
    }
    const teleport::Status& st = teleport::status();
    if (st.resultSeq == sTeleportSeq) {
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail(
                fmt::format("clickPlayersWidget {}: no teleport result ({})", target, st.message));
        }
        return false;
    }
    const std::string got = teleport::resultName(st.lastResult);
    const std::string reason = step.value("reason", std::string{});
    if (!resultMatches(step, got) || (!reason.empty() && reason != st.reason)) {
        ctx.fail(fmt::format(
            "clickPlayersWidget {}: got {} '{}' ({})", target, got, st.reason, st.message));
        return false;
    }
    TwiliLog.info("[autotest] players widget teleport result={} ({})", got, st.message);
    return true;
}

// SDL mouse events: press at `from`, move to `to` over `frames` ticks, release.
std::optional<bool> mouseDrag(StepContext& ctx) {
    const json& step = ctx.step;
    const json from = step.value("from", json::array({0, 0}));
    const json to = step.value("to", json::array({0, 0}));
    const int frames = std::max(2, step.value("frames", 10));
    const int t = ctx.begun ? ctx.ticks - sDrag.value("start", 0) : 0;
    if (!ctx.begun) {
        sDrag = {{"start", ctx.ticks}};
    }
    const auto at = [&](float f) {
        return std::pair{
            static_cast<int>(std::lround(
                from[0].get<float>() + (to[0].get<float>() - from[0].get<float>()) * f)),
            static_cast<int>(std::lround(
                from[1].get<float>() + (to[1].get<float>() - from[1].get<float>()) * f))};
    };
    bool posted = true;
    if (t == 0) {
        const auto [x, y] = at(0.0f);
        posted = input::mouseMove(x, y, false);
    } else if (t == 1) {
        const auto [x, y] = at(0.0f);
        posted = input::mouseButton(x, y, true);
    } else if (t <= frames + 1) {
        const auto [x, y] = at(static_cast<float>(t - 1) / static_cast<float>(frames));
        posted = input::mouseMove(x, y, true);
    } else if (t == frames + 2) {
        const auto [x, y] = at(1.0f);
        posted = input::mouseButton(x, y, false);
    } else {
        return t >= frames + 6;
    }
    if (!posted) {
        ctx.fail("mouseDrag: no game window");
    }
    return false;
}

// Drags the widget out of the top left corner by `by` and works out its size from where the host
// left it: the host spreads 0..1 over the free space, so x = by.x / (area.w - width).
std::optional<bool> measurePlayersWidget(StepContext& ctx) {
    constexpr int kSettle = 3;
    constexpr int kFrames = 8;
    const json& step = ctx.step;
    const float gx = step.value("grab", json::array({20, 20}))[0].get<float>();
    const float gy = step.value("grab", json::array({20, 20}))[1].get<float>();
    const float dx = step.value("by", json::array({100, 50}))[0].get<float>();
    const float dy = step.value("by", json::array({100, 50}))[1].get<float>();
    if (!ctx.begun) {
        const widget::Probe& p = widget::probe();
        sMeasure = {.start = ctx.ticks, .origX = p.x, .origY = p.y};
        widget::moveForTest(0.0f, 0.0f);
        sMeasure.moves = widget::probe().moves;
    }
    const int t = ctx.ticks - sMeasure.start;
    const auto at = [&](float f) {
        return std::pair{
            static_cast<int>(std::lround(gx + dx * f)), static_cast<int>(std::lround(gy + dy * f))};
    };
    bool posted = true;
    if (t < kSettle) {
        return false;
    } else if (t == kSettle) {
        posted = input::mouseMove(at(0.0f).first, at(0.0f).second, false);
    } else if (t == kSettle + 1) {
        posted = input::mouseButton(at(0.0f).first, at(0.0f).second, true);
    } else if (t <= kSettle + 1 + kFrames) {
        const auto [x, y] = at(static_cast<float>(t - kSettle - 1) / kFrames);
        posted = input::mouseMove(x, y, true);
    } else if (t == kSettle + 2 + kFrames) {
        posted = input::mouseButton(at(1.0f).first, at(1.0f).second, false);
    } else {
        const widget::Probe& p = widget::probe();
        if (p.moves == sMeasure.moves) {
            if (ctx.seconds > ctx.timeout(5.0)) {
                ctx.fail("measurePlayersWidget: the drag did not move the widget");
            }
            return false;
        }
        if (p.movedX <= 0.001f || p.movedX >= 0.999f || p.movedY <= 0.001f || p.movedY >= 0.999f) {
            ctx.fail(fmt::format("measurePlayersWidget: dragged to {:.3f},{:.3f}, not inside the "
                                 "free space",
                p.movedX, p.movedY));
            return false;
        }
        const json area = step.value("area", json::array({776, 426}));
        const float w = area[0].get<float>() - dx / p.movedX;
        const float h = area[1].get<float>() - dy / p.movedY;
        widget::moveForTest(static_cast<float>(sMeasure.origX) / 1000.0f,
            static_cast<float>(sMeasure.origY) / 1000.0f);
        TwiliLog.info("[autotest] players widget measured {:.1f}x{:.1f} at {}%", w, h, p.scale);
        if (step.contains("as")) {
            sSizes[step.value("as", std::string{})] = {w, h};
        }
        if (step.contains("of")) {
            const auto it = sSizes.find(step.value("of", std::string{}));
            const json ratio = step.value("ratio", json::array({0.0, 1e9}));
            const float lo = ratio[0].get<float>();
            const float hi = ratio[1].get<float>();
            if (it == sSizes.end()) {
                ctx.fail("measurePlayersWidget: nothing measured as '" +
                         step.value("of", std::string{}) + "'");
                return false;
            }
            const float rw = w / it->second.first;
            const float rh = h / it->second.second;
            if (rw < lo || rw > hi || rh < lo || rh > hi) {
                ctx.fail(fmt::format(
                    "measurePlayersWidget: {:.1f}x{:.1f} is {:.2f}x{:.2f} of {:.1f}x{:.1f}", w, h,
                    rw, rh, it->second.first, it->second.second));
                return false;
            }
            TwiliLog.info("[autotest] players widget size ratio {:.2f}x{:.2f}", rw, rh);
        }
        return true;
    }
    if (!posted) {
        ctx.fail("measurePlayersWidget: no game window");
    }
    return false;
}

std::optional<bool> playersWidgetSteps(const std::string& op, StepContext& ctx) {
    if (op == "expectPlayersWidget") {
        return expectPlayersWidget(ctx);
    }
    if (op == "expectPlayersTab") {
        return expectPlayersTab(ctx);
    }
    if (op == "clickPlayersWidget") {
        return clickPlayersWidget(ctx);
    }
    if (op == "movePlayersWidget") {
        widget::moveForTest(ctx.step.value("x", 0.0f), ctx.step.value("y", 0.0f));
        return true;
    }
    if (op == "mouseDrag") {
        return mouseDrag(ctx);
    }
    if (op == "measurePlayersWidget") {
        return measurePlayersWidget(ctx);
    }
    if (op == "mouseMove") {
        if (!input::mouseMove(ctx.step.value("x", 0), ctx.step.value("y", 0), false)) {
            ctx.fail("mouseMove: no game window");
        }
        return true;
    }
    if (op == "playersWidgetOption") {
        const std::string name = ctx.step.value("name", std::string{});
        if (name == "players_widget") {
            config::setBool(config::Var::PlayersWidget, ctx.step.value("value", true));
        } else if (name == "players_widget_opacity") {
            config::setInt(config::Var::PlayersWidgetOpacity, ctx.step.value("value", 80));
        } else if (name == "players_widget_scale") {
            config::setInt(config::Var::PlayersWidgetScale, ctx.step.value("value", 100));
        } else {
            ctx.fail("playersWidgetOption: unknown option '" + name + "'");
            return false;
        }
        return true;
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&playersWidgetSteps);

}  // namespace
}  // namespace twili::autotest
