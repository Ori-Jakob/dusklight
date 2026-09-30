#include "core/Host.hpp"

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "d/actor/d_a_alink.h"

#include <mods/svc/gfx.h>

#include <cstdio>
#include <fmt/format.h>
#include <tuple>

namespace twili::host {

bool requestQuit(int exitCode) {
    if (!SERVICE_HAS(svc_host, HostService, request_quit) || svc_host->request_quit == nullptr) {
        return false;
    }
    svc_host->request_quit(mod_ctx, exitCode);
    return true;
}

std::string versionProblem() {
    const char* found = svc_host->version != nullptr ? svc_host->version : "";
    int major = 0;
    int minor = 0;
    int patch = 0;
    const bool parsed = std::sscanf(found, "%d.%d.%d", &major, &minor, &patch) == 3;
    // Untagged builds report 0.0.0.0. GfxService 1.4 came with 2.0.3, so an older build lacks it
    // whatever version it reports (an old fork build had another InterpService layout).
    if (svc_gfx != nullptr &&
        parsed && (std::tie(major, minor, patch) >= std::make_tuple(2, 0, 3) ||
                      (major == 0 && minor == 0 && patch == 0)))
    {
        return {};
    }
    return fmt::format("Twili-Together needs Dusklight 2.0.3 or newer (found {})",
        *found != '\0' ? found : "no version");
}

}  // namespace twili::host

namespace twili::interp {

void snapModels(daAlink_c& link) {
    J3DModel* const models[] = {link.mpLinkModel, link.mpLinkFaceModel, link.mpLinkHatModel,
        link.mpLinkHandModel, link.mSwordModel, link.mSheathModel, link.mShieldModel,
        link.mHeldItemModel, link.mpHookTipModel, link.mpKanteraModel, link.mpLinkBootModels[0],
        link.mpLinkBootModels[1]};
    for (J3DModel* model : models) {
        if (model != nullptr) {
            model->forgetMtx();
        }
    }
}

}  // namespace twili::interp
