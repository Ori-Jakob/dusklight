#include "actors/Profiles.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/GameAccess.hpp"
#include "core/Log.hpp"

#include <mods/svc/actor.h>

extern const ActorService* svc_actor;

namespace twili::actors {

ModResult registerProfiles() {
    ProfileName name = 0;
    ActorHandle handle = 0;
    const ModResult result =
        svc_actor->register_actor(mod_ctx, &g_dummyPlayerProfile, &name, &handle);
    if (result != MOD_OK) {
        TwiliLog.error("[actors] registering {} failed ({})", g_dummyPlayerProfile.name,
            static_cast<int>(result));
        return result;
    }
    g_procDummyPlayer = name;
    TwiliLog.info("[actors] {} is proc {} ({} bytes)", g_dummyPlayerProfile.name, name,
        g_dummyPlayerProfile.process_size);
    return MOD_OK;
}

// The host unregisters the profiles itself after mod_shutdown.
void forgetProfiles() {
    g_procDummyPlayer = -1;
    g_procDummyHorse = -1;
}

}  // namespace twili::actors
