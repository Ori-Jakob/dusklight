// Steps for world sync and room options; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"

#include <fmt/format.h>

namespace twili::autotest {
namespace {

using config::Var;

struct RoomOption {
    const char* name;
    Var var;
    bool RoomState::* field;
};

constexpr RoomOption kRoomOptions[] = {
    {"syncWorldState", Var::SyncWorldState, &RoomState::syncWorldState},
    {"shareWoodenShield", Var::ShareWoodenShield, &RoomState::shareWoodenShield},
    {"teleportMode", Var::TeleportMode, &RoomState::teleportMode},
    {"hidePlayersInCutscene", Var::HidePlayersInCutscene, &RoomState::hidePlayersInCutscene},
    {"syncNPCs", Var::SyncEnemyDeaths, &RoomState::syncNPCs},
    {"syncEnemyDamage", Var::SyncEnemyDamage, &RoomState::syncEnemyDamage},
    {"pvpMode", Var::PvpMode, &RoomState::pvpMode},
    {"pvpFriendlyFire", Var::PvpFriendlyFire, &RoomState::pvpFriendlyFire},
    {"pvpLethal", Var::PvpLethal, &RoomState::pvpLethal},
    {"showLocationsMode", Var::ShowLocations, &RoomState::showLocationsMode},
    {"cutsceneSync", Var::CutsceneSync, &RoomState::cutsceneSync},
};

const RoomOption* findRoomOption(const std::string& name) {
    for (const RoomOption& option : kRoomOptions) {
        if (name == option.name) {
            return &option;
        }
    }
    return nullptr;
}

std::optional<bool> worldSteps(const std::string& op, StepContext& ctx) {
    const nlohmann::json& step = ctx.step;

    // Our room-setting default; the owner's value is the room's, so every instance sets it.
    if (op == "setRoomOption" || op == "waitRoomOption") {
        const std::string name = step.value("name", std::string{});
        const bool value = step.value("value", true);
        const RoomOption* option = findRoomOption(name);
        if (option == nullptr) {
            ctx.fail("unknown room option '" + name + "'");
            return false;
        }
        if (op == "setRoomOption") {
            config::setBool(option->var, value);
            return true;
        }
        if (Session::instance().roomState().*(option->field) == value) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(90.0)) {
            ctx.fail(fmt::format("waitRoomOption {} never became {}", name, value));
        }
        return false;
    }

    // Before connect: announce another save layout, as a different game build would.
    if (op == "forceLayout") {
        sync::setLayoutOverrideForTest(step.value("layout", std::string("00000000deadbeef")));
        return true;
    }

    if (op == "waitLayoutMismatch") {
        const std::string peer = step.value("peer", std::string{});
        if (sync::layoutWarned(peer)) {
            TwiliLog.info("[autotest] layout mismatch reported for {}", peer);
            return true;
        }
        if (ctx.seconds > ctx.timeout(60.0)) {
            ctx.fail("no layout-mismatch warning for " + peer);
        }
        return false;
    }

    // The item stays missing for forSec (default 5).
    if (op == "expectNoItem") {
        const int item = step.value("item", 0);
        if (dComIfGs_isItemFirstBit(static_cast<u8>(item))) {
            ctx.fail(fmt::format("item 0x{:02X} arrived", item));
            return false;
        }
        return ctx.seconds >= step.value("forSec", 5.0);
    }

    // World-state merges applied so far: at least `min`, at most `max`.
    if (op == "expectMerges") {
        const uint32_t merges = sync::stats().merges;
        const int max = step.value("max", -1);
        if (max >= 0 && merges > static_cast<uint32_t>(max)) {
            ctx.fail(fmt::format("{} world-state merge(s), want at most {}", merges, max));
            return false;
        }
        if (merges >= static_cast<uint32_t>(step.value("min", 0))) {
            return true;
        }
        if (ctx.seconds > ctx.timeout(30.0)) {
            ctx.fail(fmt::format("only {} world-state merge(s)", merges));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&worldSteps);

}  // namespace
}  // namespace twili::autotest
