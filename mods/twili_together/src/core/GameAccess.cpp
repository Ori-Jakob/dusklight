#include "core/GameAccess.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"

namespace twili {

int16_t g_procDummyPlayer = -1;
int16_t g_procDummyHorse = -1;

bool isDummyPlayer(const fopAc_ac_c* actor) {
    return actor != nullptr && g_procDummyPlayer >= 0 &&
           fopAcM_GetName(const_cast<fopAc_ac_c*>(actor)) == g_procDummyPlayer;
}

daAlink_c* localLink() {
    return daAlink_getAlinkActorClass();
}

}  // namespace twili
