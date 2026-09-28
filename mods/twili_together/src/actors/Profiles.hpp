#pragma once

#include <mods/api.h>

namespace twili::actors {

// Registers the puppet profiles with ActorService and records their proc names.
ModResult registerProfiles();
void forgetProfiles();

}  // namespace twili::actors
