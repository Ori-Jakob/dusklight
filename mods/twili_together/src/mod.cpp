#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/host.h"
#include "mods/svc/interp.h"
#include "mods/svc/log.h"
#include "mods/svc/net.h"
#include "mods/svc/ui.h"
#include "mods/svc/websocket.h"

#include "actors/DummyPlayer.hpp"
#include "actors/Profiles.hpp"
#include "core/Config.hpp"
#include "core/Layout.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "hooks/Hooks.hpp"
#include "ui/TwiliWindow.hpp"

#if TWILI_ENABLE_AUTOTEST
#include "autotest/AutoTest.hpp"
#endif

#include <string>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
// Older minors: request_quit and find_host_var are checked with SERVICE_HAS.
IMPORT_SERVICE_VERSION(HostService, svc_host, 2);
IMPORT_SERVICE_VERSION(ConfigService, svc_config, 0);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_OPTIONAL_SERVICE(InterpService, svc_interp);
// Either transport may be missing on a platform; the URL scheme picks one.
IMPORT_OPTIONAL_SERVICE(WebSocketService, svc_websocket);
IMPORT_OPTIONAL_SERVICE(NetService, svc_net);

namespace {

const char* yesNo(bool value) {
    return value ? "yes" : "no";
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    using namespace twili;

    TwiliLog.info("[core] Twili-Together {} starting (save layout {}; host: interp={} "
                  "request_quit={} find_host_var={} websocket={} net={})",
        svc_host->mod_version(mod_ctx), layout::saveLayoutHex(), yesNo(svc_interp != nullptr),
        yesNo(SERVICE_HAS(svc_host, HostService, request_quit)),
        yesNo(SERVICE_HAS(svc_config, ConfigService, find_host_var)),
        yesNo(svc_websocket != nullptr), yesNo(svc_net != nullptr));

    ModResult result = config::registerAll();
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to register settings");
    }

    result = actors::registerProfiles();
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to register the remote player actor");
    }

    Session::init();

    std::string hookError;
    result = hooks::install(hooks::Group::Core, hookError);
    if (result != MOD_OK) {
        return mods::set_error(error, result, ("core hooks: " + hookError).c_str());
    }

    if (ui::registerMenu() != MOD_OK) {
        TwiliLog.warn("[ui] could not add the menu tab");
    }

#if TWILI_ENABLE_AUTOTEST
    autotest::init();
    if (autotest::isActive()) {
        result = hooks::install(hooks::Group::Autotest, hookError);
        if (result != MOD_OK) {
            return mods::set_error(error, result, ("autotest hooks: " + hookError).c_str());
        }
        autotest::installNetDriver();
    }
#endif

    TwiliLog.info("[core] Twili-Together initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    twili::Session::instance().update();
#if TWILI_ENABLE_AUTOTEST
    twili::autotest::tick();
#endif
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    using namespace twili;
#if TWILI_ENABLE_AUTOTEST
    autotest::removeNetDriver();
#endif
    ui::shutdown();
    Session::shutdown();
    // Our actors are gone by now; make sure the local Link owns the audio again.
    restoreLocalLinkAudioPtr();
    actors::forgetProfiles();
    hooks::Scope::clear();
    TwiliLog.info("[core] Twili-Together shut down");
    return MOD_OK;
}
}
