// Steps for the item a player holds up (item gets); reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/GetItem.hpp"

#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cmath>
#include <cstring>

namespace twili::autotest {
namespace {

using nlohmann::json;

int sMatchedAt = -1;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

fopAc_ac_c* firstPeerDummy() {
    auto& session = Session::instance();
    for (const auto& [id, c] : session.clients()) {
        if (peerInMyLayer(c)) {
            if (fopAc_ac_c* dummy = session.dummyActorForClient(id)) return dummy;
        }
    }
    return nullptr;
}

// Empty when the dummy's held up item is as the step says.
std::string remoteMismatch(const json& step, const DummyGetItemDebug& g) {
    if (step.value("none", false)) {
        if (g.wanted != 0xFF || g.item != 0xFF || g.loading || g.placed) {
            return fmt::format("still holds item 0x{:02X} up (built 0x{:02X}, loading {}, drawn {})",
                               g.wanted, g.item, g.loading, g.placed);
        }
        return {};
    }
    const int item = step.value("item", -1);
    // Held up there but not drawn here (a hidden dummy)
    if (!step.value("drawn", true)) {
        if (!g.held || (item >= 0 && g.wanted != item) || g.placed) {
            return fmt::format("wanted 0x{:02X}, held {}, drawn {}", g.wanted, g.held, g.placed);
        }
        return {};
    }
    if (!g.placed) {
        return fmt::format("nothing drawn (wanted 0x{:02X}, built 0x{:02X}, loading {}, "
                           "failures {})", g.wanted, g.item, g.loading, g.failures);
    }
    if (item >= 0 && g.item != item) {
        return fmt::format("item 0x{:02X} drawn, want 0x{:02X}", g.item, item);
    }
    if (step.contains("near")) {
        const json& near = step["near"];
        const float tol = step.value("tol", 30.0f);
        float d2 = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float d = g.offset[i] - near[i].get<float>();
            d2 += d * d;
        }
        if (std::sqrt(d2) > tol) {
            return fmt::format("drawn at ({:.0f}, {:.0f}, {:.0f}) from the dummy, want within {:.0f} "
                               "of ({:.0f}, {:.0f}, {:.0f})", g.offset[0], g.offset[1],
                               g.offset[2], tol, near[0].get<float>(), near[1].get<float>(),
                               near[2].get<float>());
        }
    }
    if (g.scale < step.value("minScale", 0.0f)) {
        return fmt::format("scale {:.2f}", g.scale);
    }
    if (g.emitters < step.value("emitters", 0)) {
        return fmt::format("{} effect emitters", g.emitters);
    }
    if (g.failures > step.value("maxFailures", 0u)) {
        return fmt::format("{} items could not be shown", g.failures);
    }
    return {};
}

// Holds for `frames` ticks once it matches; `none` waits for the model to go.
bool expectRemoteGetItem(StepContext& ctx) {
    if (!ctx.begun) {
        sMatchedAt = -1;
    }
    fopAc_ac_c* dummy = firstPeerDummy();
    DummyPlayerDebugInfo info;
    const bool found = dummy != nullptr && GetDummyPlayerDebugInfo(dummy, info);
    const std::string why =
        found ? remoteMismatch(ctx.step, info.getItem) : "no peer dummy in our layer";
    if (why.empty()) {
        if (sMatchedAt < 0) {
            sMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sMatchedAt < ctx.step.value("frames", 0)) {
            return false;
        }
        const DummyGetItemDebug& g = info.getItem;
        TwiliLog.info("[autotest] remote held up item 0x{:02X}: drawn {} ticks, at ({:.1f}, "
                      "{:.1f}, {:.1f}) scale {:.2f}, emitters {}, builds {}, failures {}, heap "
                      "0x{:X}", g.item, g.placedTicks, g.offset[0], g.offset[1], g.offset[2],
                      g.scale, g.emitters, g.builds, g.failures, g.heapUsed);
        return true;
    }
    if (sMatchedAt >= 0 && !ctx.step.value("none", false)) {
        ctx.fail(fmt::format("expectRemoteGetItem: {} ticks after it matched, {}",
                             ctx.ticks - sMatchedAt, why));
        return false;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail("expectRemoteGetItem: " + why);
    }
    return false;
}

// What our own presentations sent: `item`/`real` of the latest, at least `shows` and `ticks`.
bool expectLocalGetItem(StepContext& ctx) {
    const json& step = ctx.step;
    const getitem::SenderStats& st = getitem::senderStats();
    std::string why;
    if (step.contains("item") && st.lastItem != step.value("item", 0)) {
        why = fmt::format("latest item 0x{:02X}", st.lastItem);
    } else if (step.contains("real") && st.lastRealItem != step.value("real", 0)) {
        why = fmt::format("latest given item 0x{:02X}", st.lastRealItem);
    } else if (st.shows < step.value("shows", 1u)) {
        why = fmt::format("{} items shown", st.shows);
    } else if (st.shownTicks < step.value("ticks", 0u)) {
        why = fmt::format("{} ticks shown", st.shownTicks);
    }
    if (why.empty()) {
        TwiliLog.info("[autotest] local held up item 0x{:02X} (given 0x{:02X}): {} shown, {} ticks",
                      st.lastItem, st.lastRealItem, st.shows, st.shownTicks);
        return true;
    }
    if (ctx.seconds > ctx.timeout(30.0)) {
        ctx.fail("expectLocalGetItem: " + why);
    }
    return false;
}

std::optional<bool> getItemSteps(const std::string& op, StepContext& ctx) {
    if (op == "expectRemoteGetItem") {
        return expectRemoteGetItem(ctx);
    }
    if (op == "expectLocalGetItem") {
        return expectLocalGetItem(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&getItemSteps);

}  // namespace
}  // namespace twili::autotest
