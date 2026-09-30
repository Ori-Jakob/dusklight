#pragma once

#include <mods/svc/host.h>

class daAlink_c;

namespace twili::host {

// False when the host has no request_quit.
bool requestQuit(int exitCode);

}  // namespace twili::host

namespace twili::interp {

// The next frames draw Link's models at their latest pose instead of blending across a jump.
void snapModels(daAlink_c& link);

}  // namespace twili::interp
