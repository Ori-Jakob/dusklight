// Steps for enemies spawned by the test.
//
// spawnEnemy  name (stage actor name, e.g. "E_oc"), param, dx, dy, dz, tag (name),
//             anchor (player | playerHome), setId (0xFFFF)
//     In the current room's layer at the anchor plus the offsets; playerHome is where Link
//     spawned, so instances that started at one point spawn identical enemies.
// expectEnemyHealth  tag, health, max, timeoutSec (20)
//     Until the tagged actor runs with the health and field_0x560 given.

#include "autotest/AutoTestSteps.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "f_pc/f_pc_layer.h"
#include "SSystem/SComponent/c_malloc.h"

#include <fmt/format.h>

#include <map>

namespace twili::autotest {
namespace {

// Actors created by spawnEnemy, by tag.
std::map<std::string, fpc_ProcID> sTags;

fpc_ProcID taggedId(const nlohmann::json& step) {
    const auto it = sTags.find(step.value("tag", std::string{}));
    return it != sTags.end() ? it->second : fpcM_ERROR_PROCESS_ID_e;
}

fopAc_ac_c* runningActor(fpc_ProcID id) {
    return id != fpcM_ERROR_PROCESS_ID_e && fopAcM_IsExecuting(id) ? fopAcM_SearchByID(id)
                                                                     : nullptr;
}

std::optional<bool> spawnEnemy(StepContext& ctx) {
    const std::string name = ctx.step.value("name", std::string{});
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    dStage_objectNameInf* inf = dStage_searchName(name.c_str());
    const int roomNo = dStage_roomControl_c::getStayNo();
    scene_class* room =
        roomNo >= 0 ? fopScnM_SearchByID(dStage_roomControl_c::getStatusProcID(roomNo)) : nullptr;
    if (player == nullptr || inf == nullptr || room == nullptr) {
        ctx.fail(fmt::format("spawnEnemy {}: no player, unknown actor name or no room", name));
        return false;
    }
    fopAcM_prm_class* prm = fopAcM_CreateAppend();
    if (prm == nullptr) {
        ctx.fail("spawnEnemy: out of memory");
        return false;
    }
    prm->base.parameters = static_cast<u32>(ctx.step.value("param", 0xFFFFFFFFu));
    const cXyz& anchor = ctx.step.value("anchor", std::string("player")) == "playerHome"
                             ? player->home.pos
                             : player->current.pos;
    prm->base.position = cXyz(anchor.x + ctx.step.value("dx", 0.0f),
        anchor.y + ctx.step.value("dy", 0.0f), anchor.z + ctx.step.value("dz", 0.0f));
    prm->base.setID = static_cast<u16>(ctx.step.value("setId", 0xFFFF));
    prm->room_no = static_cast<s8>(roomNo);
    prm->argument = inf->argument;
    // In the room's layer like a stage-placed actor, so it is deleted with the room.
    layer_class* saved = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(&room->base.layer);
    const fpc_ProcID id = fopAcM_Create(inf->procname, NULL, prm);
    fpcLy_SetCurrentLayer(saved);
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        cMl::free(prm);
        ctx.fail("spawnEnemy " + name + ": create request failed");
        return false;
    }
    sTags[ctx.step.value("tag", name)] = id;
    return true;
}

std::optional<bool> expectEnemyHealth(StepContext& ctx) {
    if (ctx.step.contains("tracked")) {
        ctx.fail("expectEnemyHealth: tracked needs enemy scaling (P5)");
        return false;
    }
    fopAc_ac_c* ac = runningActor(taggedId(ctx.step));
    if (ac != nullptr &&
        (!ctx.step.contains("health") || ac->health == ctx.step["health"].get<int>()) &&
        (!ctx.step.contains("max") || ac->field_0x560 == ctx.step["max"].get<int>()))
    {
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(ac == nullptr ? fmt::format("enemy '{}' is not running",
                                     ctx.step.value("tag", std::string{}))
                               : fmt::format("enemy '{}' has health {}/{}, step wants {}",
                                     ctx.step.value("tag", std::string{}), ac->health,
                                     ac->field_0x560, ctx.step.dump()));
    }
    return false;
}

std::optional<bool> enemySteps(const std::string& op, StepContext& ctx) {
    if (op == "spawnEnemy") {
        return spawnEnemy(ctx);
    }
    if (op == "expectEnemyHealth") {
        return expectEnemyHealth(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&enemySteps);

}  // namespace
}  // namespace twili::autotest
