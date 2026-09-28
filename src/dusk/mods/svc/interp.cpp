#include "registry.hpp"

#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "mods/svc/interp.h"

namespace dusk::mods::svc {
namespace {

bool interp_is_enabled(ModContext*) {
    return interp::is_enabled();
}

bool interp_is_presentation_frame(ModContext*) {
    return game_clock::is_presentation_frame();
}

float interp_presentation_step(ModContext*) {
    return interp::is_enabled() ? interp::get_interpolation_step() : 1.0f;
}

uint64_t interp_sim_tick_seq(ModContext*) {
    return interp::sim_tick_seq();
}

float interp_sim_rate_hz(ModContext*) {
    return game_clock::get_sim_rate();
}

void interp_request_presentation_sync(ModContext*) {
    interp::request_presentation_sync();
}

void interp_record_final_mtx(ModContext*, const float mtx[3][4], const void* key) {
    if (mtx == nullptr || key == nullptr) {
        return;
    }
    // The recorder only reads the matrix.
    interp::record_final_mtx(const_cast<float (*)[4]>(mtx), key);
}

void interp_forget_mtx(ModContext*, const void* key) {
    if (key != nullptr) {
        interp::forget_mtx(key);
    }
}

constexpr InterpService s_interpService{
    .header = SERVICE_HEADER(InterpService, INTERP_SERVICE_MAJOR, INTERP_SERVICE_MINOR),
    .is_enabled = interp_is_enabled,
    .is_presentation_frame = interp_is_presentation_frame,
    .presentation_step = interp_presentation_step,
    .sim_tick_seq = interp_sim_tick_seq,
    .sim_rate_hz = interp_sim_rate_hz,
    .request_presentation_sync = interp_request_presentation_sync,
    .record_final_mtx = interp_record_final_mtx,
    .forget_mtx = interp_forget_mtx,
};

}  // namespace

constinit const ServiceModule g_interpModule{
    .id = INTERP_SERVICE_ID,
    .majorVersion = INTERP_SERVICE_MAJOR,
    .minorVersion = INTERP_SERVICE_MINOR,
    .service = &s_interpService,
};

}  // namespace dusk::mods::svc
