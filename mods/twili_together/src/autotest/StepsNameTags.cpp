// Steps for name tags; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "ui/NameTags.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cmath>
#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace tags = ui::name_tags;

int sHoldSince = -1;

const Client* clientNamed(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.name == name) return &c;
    }
    return nullptr;
}

std::string describe() {
    std::string s = fmt::format("frames={} gate='{}' tags=[", tags::frameCount(), tags::lastGate());
    for (const tags::Probe& p : tags::drawn()) {
        s += fmt::format(" #{} {} '{}'{} at ({:.0f},{:.0f}) w {:.0f} #{:02X}{:02X}{:02X} hp {}/{}",
            p.clientId, p.kind == tags::Probe::Kind::Player ? "tag" : "card", p.line1,
            p.line2.empty() ? "" : " / '" + p.line2 + "'", p.x, p.y, p.width, p.r, p.g, p.b, p.life,
            p.maxLife);
    }
    return s + " ]";
}

bool held(bool ok, const StepContext& ctx, int holdTicks) {
    if (!ctx.begun) sHoldSince = -1;
    if (!ok) {
        sHoldSince = -1;
        return false;
    }
    if (sHoldSince < 0) sHoldSince = ctx.ticks;
    return ctx.ticks - sHoldSince >= holdTicks;
}

std::optional<bool> expectNameTag(StepContext& ctx) {
    const json& step = ctx.step;
    const std::string name = step.value("name", std::string{});
    const Client* c = clientNamed(name);
    const bool wantShown = step.value("shown", true);
    const tags::Probe* found = nullptr;
    for (const tags::Probe& p : tags::drawn()) {
        if (c != nullptr && p.clientId == c->clientId && p.kind == tags::Probe::Kind::Player) {
            found = &p;
        }
    }
    bool ok = c != nullptr && (found != nullptr) == wantShown;
    if (ok && found != nullptr && step.contains("line2")) {
        ok = found->line2 == step.value("line2", std::string{});
    }
    if (ok && found != nullptr && step.contains("rgb")) {
        ok = fmt::format("#{:02X}{:02X}{:02X}", found->r, found->g, found->b) ==
             step.value("rgb", std::string{});
    }
    // Quarter hearts; `health: false` wants no bar.
    if (ok && found != nullptr && step.contains("health")) {
        ok = (found->maxLife > 0) == step.value("health", true);
    }
    if (ok && found != nullptr && step.contains("life")) {
        ok = found->maxLife > 0 && found->life == step.value("life", 0);
    }
    if (ok && found != nullptr && step.contains("maxLife")) {
        ok = found->maxLife == step.value("maxLife", 0);
    }
    if (ok && found != nullptr && step.contains("maxWidth")) {
        ok = found->width <= step.value("maxWidth", 0.0f);
    }
    if (ok && !wantShown && step.contains("gate")) {
        ok = step.value("gate", std::string{}) == tags::lastGate();
    }
    if (held(ok, ctx, step.value("holdTicks", 0))) {
        TwiliLog.info("[autotest] name tags ok: {}", describe());
        return true;
    }
    if (ctx.seconds > ctx.timeout(10.0)) {
        ctx.fail(fmt::format("expectNameTag {}: {}", step.dump(), describe()));
    }
    return false;
}

std::optional<bool> viewPeer(StepContext& ctx) {
    const json& step = ctx.step;
    Session& session = Session::instance();
    const Client* c = clientNamed(step.value("name", std::string{}));
    fopAc_ac_c* dummy = c != nullptr ? session.dummyActorForClient(c->clientId) : nullptr;
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    view_class* view = dComIfGd_getView();
    if (dummy == nullptr || player == nullptr || view == nullptr) {
        ctx.fail("viewPeer: no peer dummy, player or view");
        return false;
    }
    // `beyond` past the peer on a line `yaw` degrees off: the peer shows that far off centre.
    const cXyz eye = view->lookat.eye;
    f32 dx = dummy->current.pos.x - eye.x;
    f32 dz = dummy->current.pos.z - eye.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 1.0f) {
        ctx.fail("viewPeer: the peer is at the camera");
        return false;
    }
    dx /= dist;
    dz /= dist;
    const f32 yaw = step.value("yaw", 0.0f) * 3.14159265f / 180.0f;
    const f32 ux = dx * std::cos(yaw) + dz * std::sin(yaw);
    const f32 uz = -dx * std::sin(yaw) + dz * std::cos(yaw);
    const f32 reach = dist + step.value("beyond", 200.0f);
    cXyz pos(eye.x + ux * reach, dummy->current.pos.y, eye.z + uz * reach);
    player->setPlayerPosAndAngle(&pos, cM_atan2s(ux, uz), TRUE);
    TwiliLog.info("[autotest] viewing {} from {:.0f} {:.0f} {:.0f}", c->name, pos.x, pos.y, pos.z);
    return true;
}

std::optional<bool> nameTagSteps(const std::string& op, StepContext& ctx) {
    if (op == "expectNameTag") return expectNameTag(ctx);
    if (op == "viewPeer") return viewPeer(ctx);
    if (op == "showPlayerHealth") {
        config::setBool(config::Var::ShowPlayerHealth, ctx.step.value("value", true));
        return true;
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&nameTagSteps);

}  // namespace
}  // namespace twili::autotest
