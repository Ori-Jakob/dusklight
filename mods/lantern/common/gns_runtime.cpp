#include "gns_runtime.hpp"

#include <steam/steamnetworkingsockets.h>

#include <mutex>

namespace lantern::net {
namespace {

std::mutex g_runtime_mutex;
size_t g_runtime_references = 0;

}  // namespace

GnsRuntime::~GnsRuntime() {
    shutdown();
}

bool GnsRuntime::initialize(std::string& error) {
    std::lock_guard lock(g_runtime_mutex);
    if (initialized_) {
        return true;
    }
    if (g_runtime_references == 0) {
        SteamDatagramErrMsg message{};
        if (!GameNetworkingSockets_Init(nullptr, message)) {
            error = message;
            return false;
        }
    }
    ++g_runtime_references;
    initialized_ = true;
    return true;
}

void GnsRuntime::shutdown() {
    std::lock_guard lock(g_runtime_mutex);
    if (!initialized_) {
        return;
    }
    if (--g_runtime_references == 0) {
        GameNetworkingSockets_Kill();
    }
    initialized_ = false;
}

}  // namespace lantern::net
