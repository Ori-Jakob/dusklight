#include "core/Host.hpp"

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "d/actor/d_a_alink.h"

namespace twili::host {

bool requestQuit(int exitCode) {
    if (!SERVICE_HAS(svc_host, HostService, request_quit) || svc_host->request_quit == nullptr) {
        return false;
    }
    svc_host->request_quit(mod_ctx, exitCode);
    return true;
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
