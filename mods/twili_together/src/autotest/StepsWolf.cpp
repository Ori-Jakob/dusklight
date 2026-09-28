// Steps for the local player's form.
//
// transform  form (wolf | human), timeoutSec (30)
//     Starts the transformation until it takes; done once Link is in that form.
// expectLocalForm  form

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"

#include "d/actor/d_a_alink.h"

#include <fmt/format.h>

namespace twili::autotest {
namespace {

bool wantsWolf(StepContext& ctx, bool& wolf) {
    const std::string form = ctx.step.value("form", std::string("wolf"));
    if (form != "wolf" && form != "human") {
        ctx.fail(fmt::format("{}: unknown form '{}'", ctx.step.value("op", std::string{}), form));
        return false;
    }
    wolf = form == "wolf";
    return true;
}

bool transform(StepContext& ctx) {
    bool wantWolf = true;
    if (!wantsWolf(ctx, wantWolf)) {
        return false;
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link != nullptr) {
        const bool isWolf = link->checkWolf() != 0;
        const bool transforming = link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
                                  link->getClothesChangeWaitTimer() != 0;
        if (isWolf == wantWolf && !transforming) {
            TwiliLog.info("[autotest] local player is {}", wantWolf ? "a wolf" : "human");
            return true;
        }
        // Again on every call until it takes: some procs and running events refuse it.
        if (!transforming) {
            link->procCoMetamorphoseInit();
        }
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail(link == nullptr ? std::string("transform: no player")
                                 : fmt::format("transform: still {} (proc {})",
                                       link->checkWolf() ? "a wolf" : "human",
                                       static_cast<int>(link->mProcID)));
    }
    return false;
}

std::optional<bool> wolfSteps(const std::string& op, StepContext& ctx) {
    if (op == "transform") {
        return transform(ctx);
    }
    if (op == "expectLocalForm") {
        bool wolf = true;
        if (!wantsWolf(ctx, wolf)) {
            return false;
        }
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr || (link->checkWolf() != 0) != wolf) {
            ctx.fail(fmt::format("expectLocalForm: the local player is {}",
                link == nullptr ? "missing" : link->checkWolf() ? "a wolf" : "human"));
            return false;
        }
        return true;
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&wolfSteps);

}  // namespace
}  // namespace twili::autotest
