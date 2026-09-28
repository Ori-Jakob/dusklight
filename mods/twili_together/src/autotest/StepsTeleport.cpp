// Steps for teleporting to a player; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"
#include "teleport/Teleport.hpp"

#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cmath>
#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;

// teleport::status().resultSeq when the running teleportTo step sent its request.
uint32_t sTeleportSeq = 0;

const Client* findPeer(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.online && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return c.online && c.isSaveLoaded && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

bool resultMatches(const json& step, const std::string& got, const std::string& reason) {
    const json expect = step.value("expect", json("arrived"));
    const auto one = [&](const json& e) {
        if (!e.is_string()) {
            return false;
        }
        const std::string want = e.get<std::string>();
        return want == got || (want == "arrived" && (got == "local" || got == "stage"));
    };
    bool ok = false;
    if (expect.is_array()) {
        for (const auto& e : expect) {
            ok = ok || one(e);
        }
    } else {
        ok = one(expect);
    }
    const std::string wantReason = step.value("reason", std::string{});
    return ok && (wantReason.empty() || wantReason == reason);
}

std::optional<bool> teleportSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "teleportTo") {
        const std::string target = step.value("target", std::string{});
        if (!ctx.begun) {
            const Client* c = findPeer(target);
            if (c == nullptr) {
                if (resultMatches(step, "refused", "offline")) {
                    return true;
                }
                ctx.fail("teleportTo: no online peer named '" + target + "'");
                return false;
            }
            sTeleportSeq = teleport::status().resultSeq;
            // A local refusal is reported as a result too.
            teleport::request(c->clientId);
        }
        const teleport::Status& st = teleport::status();
        if (st.resultSeq == sTeleportSeq) {
            if (ctx.seconds > ctx.timeout(90.0)) {
                ctx.fail(fmt::format("teleportTo {} timed out after {:.0f}s ({})", target,
                    ctx.timeout(90.0), st.message));
            }
            return false;
        }
        const std::string got = teleport::resultName(st.lastResult);
        TwiliLog.info("[autotest] teleport result={} reason='{}' ({})", got, st.reason, st.message);
        if (!resultMatches(step, got, st.reason)) {
            ctx.fail(
                fmt::format("teleportTo {}: got {} '{}' ({})", target, got, st.reason, st.message));
            return false;
        }
        return true;
    }

    if (op == "expectNear") {
        const std::string target = step.value("target", std::string{});
        const float maxDist = step.value("maxDist", 250.0f);
        const Client* c = findPeer(target);
        fopAc_ac_c* player = dComIfGp_getPlayer(0);
        float dist = -1.0f;
        if (c != nullptr && player != nullptr && !dComIfGp_isEnableNextStage() &&
            peerInMyLayer(*c) && c->hasPlayerUpdate)
        {
            const float dx = player->current.pos.x - c->posX;
            const float dy = player->current.pos.y - c->posY;
            const float dz = player->current.pos.z - c->posZ;
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist <= maxDist) {
                TwiliLog.info("[autotest] {:.0f} units from {}", dist, target);
                return true;
            }
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("expectNear {}: {} (max {:.0f})", target,
                dist < 0 ? std::string("peer not visible in our layer") :
                           fmt::format("{:.0f} units away", dist),
                maxDist));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&teleportSteps);

}  // namespace
}  // namespace twili::autotest
