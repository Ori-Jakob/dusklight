#pragma once

#include <mods/svc/host.h>
#include <mods/svc/interp.h>

#include <cstdint>

namespace twili::host {

// False when the host has no request_quit.
bool requestQuit(int exitCode);

const InterpService* interp();

}  // namespace twili::host

// InterpService with fallbacks for hosts without it.
namespace twili::interp {

bool isEnabled();
void requestPresentationSync();
float presentationStep();
uint64_t simTickSeq();
// Seconds per simulation tick.
float simPace();
void recordFinalMtx(const float mtx[3][4], const void* key);
void forgetMtx(const void* key);

}  // namespace twili::interp
