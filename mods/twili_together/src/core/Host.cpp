#include "core/Host.hpp"

namespace twili::host {

bool requestQuit(int exitCode) {
    if (!SERVICE_HAS(svc_host, HostService, request_quit) || svc_host->request_quit == nullptr) {
        return false;
    }
    svc_host->request_quit(mod_ctx, exitCode);
    return true;
}

const InterpService* interp() {
    return svc_interp;
}

}  // namespace twili::host
