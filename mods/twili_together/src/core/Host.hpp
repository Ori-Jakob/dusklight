#pragma once

#include <mods/svc/host.h>
#include <mods/svc/interp.h>

namespace twili::host {

// False when the host has no request_quit.
bool requestQuit(int exitCode);

const InterpService* interp();

}  // namespace twili::host
