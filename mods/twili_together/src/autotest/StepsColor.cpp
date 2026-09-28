// Steps for the player colour, its live sync and the window; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "autotest/Input.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/PlayerRecolor.hpp"
#include "ui/ColorMath.hpp"
#include "ui/TwiliWindow.hpp"

#include "d/d_com_inf_game.h"

#include <fmt/format.h>
#include <mods/svc/ui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace color = ui::color;

uint32_t sSelfTestNext = 0;
uint32_t sColorPushBase = 0;
int sTaps = 0;
int sPhaseTick = 0;

std::optional<color::Rgb8> stepColor(const json& step, const char* key = "rgb") {
    if (step.contains(key) && step[key].is_string()) {
        return color::parseHex(step[key].get<std::string>());
    }
    if (step.contains("r") && step.contains("g") && step.contains("b")) {
        return color::Rgb8{static_cast<uint8_t>(step.value("r", 0)),
            static_cast<uint8_t>(step.value("g", 0)), static_cast<uint8_t>(step.value("b", 0))};
    }
    return std::nullopt;
}

color::Rgb8 settingColor() {
    const auto c = localPlayerColor();
    return {static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]), static_cast<uint8_t>(c[2])};
}

template <typename Ok, typename Why>
bool waitFor(StepContext& ctx, double timeoutSec, const char* what, Ok ok, Why why) {
    if (ok()) {
        return true;
    }
    if (ctx.seconds > ctx.timeout(timeoutSec)) {
        ctx.fail(std::string(what) + ": " + why());
    }
    return false;
}

const Client* findPeer(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.online && c.name == name) return &c;
    }
    return nullptr;
}

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

color::Rgb8 clientColor(const Client& c) {
    return {c.colorR, c.colorG, c.colorB};
}

std::string selfRowMismatch(const json& step) {
    const Client* self = Session::instance().selfClient();
    if (self == nullptr) {
        return "no own client entry";
    }
    const std::string stage(self->stageName, strnlen(self->stageName, sizeof(self->stageName)));
    const std::string state = fmt::format("saveLoaded={} stage={} layer={} color={}",
        self->isSaveLoaded, stage, self->layerNo, color::formatHex(clientColor(*self)));
    if (step.contains("saveLoaded") && step["saveLoaded"].get<bool>() != self->isSaveLoaded) {
        return "saveLoaded differs: " + state;
    }
    if (step.contains("stage") && step["stage"].get<std::string>() != stage) {
        return "stage differs: " + state;
    }
    if (const auto want = stepColor(step); want && *want != clientColor(*self)) {
        return "colour differs: " + state;
    }
    return {};
}

std::string hsvMismatch(const json& step) {
    const color::Rgb8 c = settingColor();
    const color::Hsv hsv = color::rgbToHsv(c, {0.0, 0.0, 1.0});
    const std::string state =
        fmt::format("{} = hsv({:.2f}, {:.3f}, {:.3f})", color::formatHex(c), hsv.h, hsv.s, hsv.v);
    const double hTol = step.value("hTol", 0.6), tol = step.value("tol", 0.011);
    if (step.contains("h")) {
        const double d = std::fmod(std::abs(step["h"].get<double>() - hsv.h), 360.0);
        if (std::min(d, 360.0 - d) > hTol) return "h differs: " + state;
    }
    if (step.contains("hMin") && hsv.h < step["hMin"].get<double>()) return "hMin: " + state;
    if (step.contains("hMax") && hsv.h > step["hMax"].get<double>()) return "hMax: " + state;
    if (step.contains("s") && std::abs(step["s"].get<double>() - hsv.s) > tol) {
        return "s differs: " + state;
    }
    if (step.contains("v") && std::abs(step["v"].get<double>() - hsv.v) > tol) {
        return "v differs: " + state;
    }
    return {};
}

std::optional<input::Key> stepKey(const std::string& key) {
    if (key == "left") return input::Key::Left;
    if (key == "right") return input::Key::Right;
    if (key == "up") return input::Key::Up;
    if (key == "down") return input::Key::Down;
    if (key == "confirm") return input::Key::Confirm;
    if (key == "cancel") return input::Key::Cancel;
    if (key == "next") return input::Key::Next;
    if (key == "prev") return input::Key::Prev;
    return std::nullopt;
}

bool tap(input::Key key) {
    return input::keyDown(key, false) && input::keyUp(key);
}

bool pickerKey(StepContext& ctx) {
    const auto key = stepKey(ctx.step.value("key", std::string{}));
    if (!key) {
        ctx.fail("pickerKey: unknown key");
        return false;
    }
    const double holdSec = ctx.step.value("holdMs", 0) / 1000.0;
    if (holdSec > 0.0) {
        if (!ctx.begun) {
            input::keyDown(*key, false);
            return false;
        }
        if (ctx.seconds >= holdSec) {
            input::keyUp(*key);
            return true;
        }
        if (ctx.seconds >= 0.32) {
            input::keyDown(*key, true);
        }
        return false;
    }
    if (!tap(*key)) {
        ctx.fail("pickerKey: no game window to post to");
        return false;
    }
    sTaps = ctx.begun ? sTaps + 1 : 1;
    return sTaps >= ctx.step.value("count", 1);
}

// The window up and settled on its first tab, then `tabs` next-tab taps.
bool showWindow(StepContext& ctx, int tabs) {
    if (!ctx.begun) {
        ui::closeWindow();
        sPhaseTick = -1;
        return false;
    }
    if (!ui::windowOpen()) {
        ui::openWindow();
        sPhaseTick = ctx.ticks;
        return false;
    }
    const int since = ctx.ticks - sPhaseTick;
    if (since < 12) {
        return false;
    }
    if (since < 12 + tabs) {
        tap(input::Key::Next);
        return false;
    }
    return since >= 12 + tabs + 10;
}

bool openColorPicker(StepContext& ctx) {
    if (!showWindow(ctx, 0)) {
        if (ctx.seconds > ctx.timeout(10.0)) ctx.fail("openColorPicker: the window did not open");
        return false;
    }
    const UiElementHandle control = ui::colorControl();
    if (control == 0) {
        ctx.fail("openColorPicker: no colour control on the first tab");
        return false;
    }
    const int since = ctx.ticks - sPhaseTick;
    if (since == 22) {
        svc_ui->elem_focus(mod_ctx, control);
        return false;
    }
    if (since == 24) {
        tap(input::Key::Confirm);
        return false;
    }
    return since >= 34;
}

std::optional<bool> colorSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "mark") {
        TwiliLog.warn("[autotest] MARK {}", step.value("msg", std::string{}));
        return true;
    }

    if (op == "colorMathSelfTest") {
        if (!ctx.begun) {
            sSelfTestNext = 0;
        }
        const std::string failure = color::selfTest(&sSelfTestNext, step.value("chunk", 1u << 20));
        if (!failure.empty()) {
            ctx.fail("colorMathSelfTest: " + failure);
            return false;
        }
        if (sSelfTestNext < (1u << 24)) {
            return false;
        }
        TwiliLog.info("[autotest] colour maths OK (all 16777216 colours round-trip)");
        return true;
    }

    if (op == "setColor") {
        const auto c = stepColor(step);
        if (!c) {
            ctx.fail("setColor: no colour given");
            return false;
        }
        config::setString(config::Var::Color, color::formatConfig(*c));
        TwiliLog.info("[autotest] player colour set to {}", color::formatHex(*c));
        return true;
    }

    if (op == "expectColor") {
        const auto want = stepColor(step);
        return waitFor(
            ctx, 5.0, "expectColor", [&] { return want && settingColor() == *want; },
            [&] { return "the setting holds " + color::formatHex(settingColor()); });
    }

    if (op == "expectColorHsv") {
        std::string why;
        return waitFor(
            ctx, 5.0, "expectColorHsv", [&] { return (why = hsvMismatch(step)).empty(); },
            [&] { return why; });
    }

    if (op == "showWindow") {
        if (showWindow(ctx, std::clamp(step.value("tab", 0), 0, 2))) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) ctx.fail("showWindow: the window did not open");
        return false;
    }

    if (op == "hideWindow") {
        ui::closeWindow();
        return true;
    }

    if (op == "openColorPicker") {
        return openColorPicker(ctx);
    }

    if (op == "pickerKey") {
        return pickerKey(ctx);
    }

    if (op == "closeColorPicker") {
        tap(input::Key::Cancel);
        return true;
    }

    if (op == "resizeWindow") {
        if (!ctx.begun && !input::resizeWindow(step.value("width", 800), step.value("height", 450))) {
            ctx.fail("resizeWindow: no game window");
            return false;
        }
        return ctx.seconds >= 0.5;
    }

    auto& session = Session::instance();

    if (op == "expectPeerColor") {
        const std::string name = step.value("name", std::string{});
        const auto want = stepColor(step);
        return waitFor(
            ctx, 10.0, "expectPeerColor",
            [&] {
                const Client* peer = findPeer(name);
                return want && peer != nullptr && clientColor(*peer) == *want;
            },
            [&] {
                const Client* peer = findPeer(name);
                return peer ? name + " holds " + color::formatHex(clientColor(*peer))
                            : "no peer " + name;
            });
    }

    if (op == "expectSelfRow") {
        std::string why;
        return waitFor(
            ctx, 5.0, "expectSelfRow", [&] { return (why = selfRowMismatch(step)).empty(); },
            [&] { return why; });
    }

    if (op == "expectDummyColor") {
        const std::string name = step.value("name", std::string{});
        const auto want = stepColor(step);
        std::string why = "no peer dummy in our layer";
        return waitFor(
            ctx, 10.0, "expectDummyColor",
            [&] {
                for (const auto& [id, c] : session.clients()) {
                    if (!peerInMyLayer(c) || (!name.empty() && c.name != name)) {
                        continue;
                    }
                    DummyPlayerDebugInfo info;
                    fopAc_ac_c* dummy = session.dummyActorForClient(id);
                    if (dummy == nullptr || !GetDummyPlayerDebugInfo(dummy, info) ||
                        !info.shellReady)
                    {
                        continue;
                    }
                    const color::Rgb8 have{info.colorR, info.colorG, info.colorB};
                    // Its clothes textures hold the colour too, where recolouring is on.
                    const bool recolored =
                        !info.recolorBound ||
                        (want && info.recolorKey == recolorKey(want->r, want->g, want->b));
                    if (want && have == *want && recolored) {
                        return true;
                    }
                    why = c.name + "'s dummy tints with " + color::formatHex(have) +
                          (recolored ? "" : ", its textures not yet");
                }
                return false;
            },
            [&] { return why; });
    }

    if (op == "resetColorPushes") {
        sColorPushBase = session.colorPushCount();
        return true;
    }

    if (op == "expectColorPushes") {
        const uint32_t pushes = session.colorPushCount() - sColorPushBase;
        TwiliLog.info("[autotest] colour updates sent: {}", pushes);
        if (pushes < step.value("min", 0u) || pushes > step.value("max", ~0u)) {
            ctx.fail(fmt::format("expectColorPushes: {} sent", pushes));
            return false;
        }
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&colorSteps);

}  // namespace
}  // namespace twili::autotest
