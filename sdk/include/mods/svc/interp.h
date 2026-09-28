#pragma once

#include <mods/api.h>

#ifdef __cplusplus
#include <mods/service.hpp>
#endif

/* Frame interpolation and the simulation clock. Game thread only. */

#define INTERP_SERVICE_ID DUSKLIGHT_SERVICE_ID_PREFIX "interp"
#define INTERP_SERVICE_MAJOR 1u
#define INTERP_SERVICE_MINOR 0u

typedef struct InterpService {
    ServiceHeader header;

    /* Presentation is decoupled from the simulation and interpolating. */
    bool (*is_enabled)(ModContext* ctx);
    /* False during a simulation tick. */
    bool (*is_presentation_frame)(ModContext* ctx);
    /* Blend between the previous (0) and latest (1) tick for the presented frame. */
    float (*presentation_step)(ModContext* ctx);
    /* Incremented once per simulation tick while interpolating. */
    uint64_t (*sim_tick_seq)(ModContext* ctx);
    /* 30 Hz times the game clock's time scale. */
    float (*sim_rate_hz)(ModContext* ctx);
    /* Present the next frame without blending (e.g. after a teleport). */
    void (*request_presentation_sync)(ModContext* ctx);
    /* Record a hand-written matrix once per tick; key is the address the renderer reads. */
    void (*record_final_mtx)(ModContext* ctx, const float mtx[3][4], const void* key);
    /* Drop recordings for key before its memory is freed. */
    void (*forget_mtx)(ModContext* ctx, const void* key);
} InterpService;

MOD_DECLARE_SERVICE(
    InterpService, svc_interp, INTERP_SERVICE_ID, INTERP_SERVICE_MAJOR, INTERP_SERVICE_MINOR);
