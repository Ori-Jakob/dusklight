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

namespace twili::interp {

// Without the service assume interpolation is on: the setAnmMtx path is right either way.
bool isEnabled() {
    return svc_interp == nullptr || svc_interp->is_enabled(mod_ctx);
}

void requestPresentationSync() {
    if (svc_interp != nullptr) {
        svc_interp->request_presentation_sync(mod_ctx);
    }
}

float presentationStep() {
    return svc_interp != nullptr ? svc_interp->presentation_step(mod_ctx) : 1.0f;
}

uint64_t simTickSeq() {
    return svc_interp != nullptr ? svc_interp->sim_tick_seq(mod_ctx) : 0;
}

float simPace() {
    const float rate = svc_interp != nullptr ? svc_interp->sim_rate_hz(mod_ctx) : 30.0f;
    return rate > 0.0f ? 1.0f / rate : 1.0f / 30.0f;
}

void recordFinalMtx(const float mtx[3][4], const void* key) {
    if (svc_interp != nullptr) {
        svc_interp->record_final_mtx(mod_ctx, mtx, key);
    }
}

}  // namespace twili::interp
