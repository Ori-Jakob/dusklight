// Steps for our life and standing at a peer's dummy. Life values are quarter hearts: the saved
// life plus the change the meter has not applied yet.
//
// setLife  value (12)
// markLife
//     Remembers our life for expectLifeDelta.
// expectLife  value | min | max, timeoutSec (10)
// expectLifeDelta  delta, frames (60), timeoutSec (10)
//     Until life - marked == delta; fails at once past it. delta 0 must hold for `frames` ticks.
// approachDummy  dist (110)
//     Our Link `dist` in front of the first peer's dummy, facing it, held there for 10 ticks.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "core/Session.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "SSystem/SComponent/c_math.h"

#include <fmt/format.h>

#include <cstdlib>
#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

fopAc_ac_c* firstPeerDummy() {
    auto& b = Session::instance();
    for (const auto& [id, c] : b.clients()) {
        if (peerInMyLayer(c)) {
            if (fopAc_ac_c* dummy = b.dummyActorForClient(id)) return dummy;
        }
    }
    return nullptr;
}

int currentLife() {
    return dComIfGs_getLife() + static_cast<int>(dComIfGp_getItemLifeCount());
}

int sMarkedLife = 0;

// Where the running approachDummy puts our Link, fixed on its first call.
cXyz sApproachDest = cXyz::Zero;
s16 sApproachYaw = 0;

bool approachDummy(StepContext& ctx) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    fopAc_ac_c* dummy = firstPeerDummy();
    if (link == nullptr || dummy == nullptr) {
        ctx.fail("approachDummy: no player or no dummy for a peer in our layer");
        return false;
    }
    const float dist = ctx.step.value("dist", 110.0f);
    if (!ctx.begun) {
        const s16 yaw = dummy->shape_angle.y;
        sApproachDest.set(dummy->current.pos.x + cM_ssin(yaw) * dist, dummy->current.pos.y,
            dummy->current.pos.z + cM_scos(yaw) * dist);
        sApproachYaw = static_cast<s16>(yaw + 0x8000);
        const cXyz delta = sApproachDest - link->current.pos;
        if (camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0))) {
            camera->mCamera.Reset(camera->mCamera.mCenter + delta, camera->mCamera.mEye + delta);
        }
    }
    // Held while settling: a proc that was turning Link would carry on from the old facing.
    if (ctx.ticks < 10) {
        link->setPlayerPosAndAngle(&sApproachDest, sApproachYaw, TRUE);
        link->speedF = 0.0f;
        link->mNormalSpeed = 0.0f;
        return false;
    }
    const cXyz toDummy = dummy->current.pos - link->current.pos;
    const float d = toDummy.absXZ();
    const int facingError = std::abs(static_cast<s16>(toDummy.atan2sX_Z() - link->shape_angle.y));
    TwiliLog.info("[autotest] approached dummy: {:.0f} units, facing off by 0x{:X} (proc 0x{:X}, "
                  "equip 0x{:X})",
        d, facingError, link->mProcID, static_cast<int>(link->mEquipItem));
    if (d > dist * 2.0f + 30.0f || facingError > 0x2000) {
        ctx.fail(fmt::format("approachDummy: ended {:.0f} units from the dummy, facing off by "
                             "0x{:X}",
            d, facingError));
        return false;
    }
    return true;
}

std::optional<bool> pvpSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "setLife") {
        dComIfGp_clearItemLifeCount();
        dComIfGs_setLife(static_cast<u16>(step.value("value", 12)));
        return true;
    }

    if (op == "markLife") {
        sMarkedLife = currentLife();
        TwiliLog.info("[autotest] life marked at {}", sMarkedLife);
        return true;
    }

    if (op == "expectLife") {
        const int life = currentLife();
        const bool ok = (!step.contains("value") || life == step.value("value", 0)) &&
                        (!step.contains("min") || life >= step.value("min", 0)) &&
                        (!step.contains("max") || life <= step.value("max", 0));
        if (ok) {
            TwiliLog.info("[autotest] life is {}", life);
            return true;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectLife: life is {} ({})", life, step.dump()));
        }
        return false;
    }

    if (op == "expectLifeDelta") {
        const int want = step.value("delta", 0);
        const int delta = currentLife() - sMarkedLife;
        if (want == 0) {
            if (delta != 0) {
                ctx.fail(fmt::format("expectLifeDelta: life changed by {}", delta));
                return false;
            }
            return ctx.ticks >= step.value("frames", 60);
        }
        if (delta == want) {
            TwiliLog.info("[autotest] life changed by {}", delta);
            return true;
        }
        if ((want < 0 && delta < want) || (want > 0 && delta > want)) {
            ctx.fail(fmt::format("expectLifeDelta: life changed by {}, want {}", delta, want));
            return false;
        }
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("expectLifeDelta: life changed by {}, want {}", delta, want));
        }
        return false;
    }

    if (op == "approachDummy") {
        return approachDummy(ctx);
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&pvpSteps);

}  // namespace
}  // namespace twili::autotest
