#include "actors/Profiles.hpp"

#include "actors/DummyHorse.hpp"
#include "actors/DummyPlayer.hpp"
#include "core/GameAccess.hpp"
#include "core/Log.hpp"

#include <mods/svc/actor.h>

extern const ActorService* svc_actor;

namespace twili::actors {

namespace {

ModResult registerProfile(const ActorProfileDesc& desc, int16_t& outProc) {
    ProfileName name = 0;
    ActorHandle handle = 0;
    const ModResult result = svc_actor->register_actor(mod_ctx, &desc, &name, &handle);
    if (result != MOD_OK) {
        TwiliLog.error("[actors] registering {} failed ({})", desc.name, static_cast<int>(result));
        return result;
    }
    outProc = name;
    TwiliLog.info("[actors] {} is proc {} ({} bytes)", desc.name, name, desc.process_size);
    return MOD_OK;
}

}  // namespace

ModResult registerProfiles() {
    const ModResult result = registerProfile(g_dummyPlayerProfile, g_procDummyPlayer);
    if (result != MOD_OK) {
        return result;
    }
    // Optional: without it remote horses are not shown.
    registerProfile(g_dummyHorseProfile, g_procDummyHorse);
    return MOD_OK;
}

// The host unregisters the profiles itself after mod_shutdown.
void forgetProfiles() {
    g_procDummyPlayer = -1;
    g_procDummyHorse = -1;
}

}  // namespace twili::actors
