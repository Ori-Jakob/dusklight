#pragma once

// What identifies a stage actor across game instances: its spawn data, not its process id.

#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace twili {

struct SpawnKey {
    int16_t procName = -1;
    int8_t roomNo = -1;
    uint32_t params = 0;
    uint16_t setId = 0;
    float home[3] = {};

    bool valid() const { return procName >= 0; }
    std::string text() const {
        return fmt::format("proc 0x{:X} room {} params 0x{:08X} setId 0x{:04X} home {:.0f} {:.0f} "
                           "{:.0f}",
            procName, roomNo, params, setId, home[0], home[1], home[2]);
    }
};

// From the live actor: an NPC may walk, so its home and not its position.
inline SpawnKey spawnKeyOf(const fopAc_ac_c* ac) {
    SpawnKey k;
    if (ac == nullptr) {
        return k;
    }
    k.procName = fopAcM_GetName(const_cast<fopAc_ac_c*>(ac));
    k.roomNo = fopAcM_GetHomeRoomNo(ac);
    k.params = fopAcM_GetParam(ac);
    k.setId = ac->setID;
    k.home[0] = ac->home.pos.x;
    k.home[1] = ac->home.pos.y;
    k.home[2] = ac->home.pos.z;
    return k;
}

inline bool sameSpawn(const SpawnKey& a, const SpawnKey& b, float homeTolerance) {
    return a.procName == b.procName && a.roomNo == b.roomNo && a.params == b.params &&
           a.setId == b.setId && std::fabs(a.home[0] - b.home[0]) <= homeTolerance &&
           std::fabs(a.home[1] - b.home[1]) <= homeTolerance &&
           std::fabs(a.home[2] - b.home[2]) <= homeTolerance;
}

}  // namespace twili
