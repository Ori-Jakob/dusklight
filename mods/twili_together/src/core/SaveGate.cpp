#include "core/SaveGate.hpp"

#include "core/Log.hpp"

namespace twili {
namespace {

bool s_saveLoaded = false;
uint32_t s_generation = 0;

}  // namespace

void markSaveLoaded() {
    if (!s_saveLoaded) {
        TwiliLog.info("[save] save loaded");
        s_generation++;
    }
    s_saveLoaded = true;
}

void clearSaveLoaded() {
    s_saveLoaded = false;
}

bool isSaveLoaded() {
    return s_saveLoaded;
}

uint32_t saveGeneration() {
    return s_generation;
}

}  // namespace twili
