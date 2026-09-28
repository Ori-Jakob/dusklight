#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/host.h"
#include "mods/svc/interp.h"
#include "mods/svc/log.h"

#include "core/Config.hpp"
#include "core/Layout.hpp"
#include "core/Log.hpp"
#include "hooks/Hooks.hpp"

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
IMPORT_OPTIONAL_SERVICE(InterpService, svc_interp);

namespace {

const char* yesNo(bool value) {
    return value ? "yes" : "no";
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    using namespace twili;

    TwiliLog.info("[core] Twili-Together {} starting (save layout {}; host: interp={} "
                  "request_quit={} find_host_var={})",
        svc_host->mod_version(mod_ctx), layout::saveLayoutHex(), yesNo(svc_interp != nullptr),
        yesNo(SERVICE_HAS(svc_host, HostService, request_quit)),
        yesNo(SERVICE_HAS(svc_config, ConfigService, find_host_var)));

    ModResult result = config::registerAll();
    if (result != MOD_OK) {
        return mods::set_error(error, result, "failed to register settings");
    }

    std::string hookError;
    result = hooks::install(hooks::Group::Core, hookError);
    if (result != MOD_OK) {
        return mods::set_error(error, result, ("core hooks: " + hookError).c_str());
    }

#if TWILI_ENABLE_AUTOTEST
    autotest::init();
    if (autotest::isActive()) {
        result = hooks::install(hooks::Group::Autotest, hookError);
        if (result != MOD_OK) {
            return mods::set_error(error, result, ("autotest hooks: " + hookError).c_str());
        }
    }
#endif

    TwiliLog.info("[core] Twili-Together initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
#if TWILI_ENABLE_AUTOTEST
    twili::autotest::tick();
#endif
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    twili::hooks::Scope::clear();
    twili::TwiliLog.info("[core] Twili-Together shut down");
    return MOD_OK;
}
}
