#include "core/SaveGate.hpp"

#include "core/Log.hpp"

namespace twili {
namespace {

bool s_saveLoaded = false;

}  // namespace

void markSaveLoaded() {
    if (!s_saveLoaded) {
        TwiliLog.info("[save] save loaded");
    }
    s_saveLoaded = true;
}

void clearSaveLoaded() {
    s_saveLoaded = false;
}

bool isSaveLoaded() {
    return s_saveLoaded;
}

}  // namespace twili
