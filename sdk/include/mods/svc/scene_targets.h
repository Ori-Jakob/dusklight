#pragma once

#include <mods/svc/gfx.h>

/*
 * The renderer's base G-buffer targets, for mods that read or write them alongside GfxService.
 * Import it with IMPORT_OPTIONAL_SERVICE: hosts without it leave the pointer NULL. Later minors
 * append fields and functions, gated by offsetof.
 *
 * Fog record (RGBA8Uint): rgb is the fog colour x 255; amount = (a >> 1) / 127; (a & 1) means the
 * fog colour is locked. The fog the game added to a pixel is F = rgb / 255 * amount, so the scene
 * without fog is S = L - F. Darkening effects apply their factor k to S and add F back:
 * L * k + F * (1 - k). A zero record means no fog contribution.
 *
 * Receiver code: the normal snapshot's alpha is above 1/6 on surfaces that receive screen-space
 * effects such as AO, under both the legacy normal (alpha 0 or 1) and the G-buffer (code / 3).
 *
 * Opaque mod geometry drawn at GFX_STAGE_SCENE_BEGIN or GFX_STAGE_SCENE_AFTER_TERRAIN should write
 * FOG 0 (or its own fog record), write MOTION from the mod's cached previous proj_from_world (or
 * REACTIVE 1 when it has none), and write normal alpha 0 to opt out of AO.
 */

#define SCENE_TARGETS_SERVICE_ID DUSKLIGHT_SERVICE_ID_PREFIX "scene_targets"
#define SCENE_TARGETS_SERVICE_MAJOR 1u
#define SCENE_TARGETS_SERVICE_MINOR 0u

/* Attachment index of a target the scene layout does not carry. */
#define SCENE_TARGET_NONE 0xFFFFFFFFu

/* Color attachment indices into the scene layout that matches key, or SCENE_TARGET_NONE. */
typedef struct SceneTargetsLayout {
    uint32_t struct_size;
    uint64_t key; /* GfxRenderTargetLayout.key of the scene layout */
    uint32_t motion;
    uint32_t reactive;
    uint32_t fog;
} SceneTargetsLayout;

#define SCENE_TARGETS_LAYOUT_INIT                                                                  \
    {sizeof(SceneTargetsLayout), 0u, SCENE_TARGET_NONE, SCENE_TARGET_NONE, SCENE_TARGET_NONE}

typedef struct SceneRecordsDesc {
    uint32_t struct_size;
    uint32_t fog; /* 0 or 1 */
} SceneRecordsDesc;

#define SCENE_RECORDS_DESC_INIT {sizeof(SceneRecordsDesc), 0u}

typedef struct SceneRecords {
    uint32_t struct_size;
    WGPUTextureView fog; /* RGBA8Uint fog record snapshot; NULL without a fog target */
    WGPUTexture fogTexture;
} SceneRecords;

#define SCENE_RECORDS_INIT {sizeof(SceneRecords), NULL, NULL}

typedef struct SceneTargetsService {
    ServiceHeader header;

    /* Game thread. Describes the scene layout recording uses now. */
    ModResult (*get_scene_targets)(ModContext* ctx, SceneTargetsLayout* out_layout);

    /*
     * GfxService.resolve_pass plus record snapshots. records_desc and out_records may be NULL
     * together. The fog record is valid from GFX_STAGE_SCENE_BEGIN until the first colour draw
     * after the scene window, and final at GFX_STAGE_SCENE_AFTER_OPAQUE.
     */
    ModResult (*resolve_pass)(ModContext* ctx, const GfxResolveDesc* desc,
        GfxResolvedTargets* out_targets, const SceneRecordsDesc* records_desc,
        SceneRecords* out_records);
} SceneTargetsService;

MOD_DECLARE_SERVICE(SceneTargetsService, svc_scene_targets, SCENE_TARGETS_SERVICE_ID,
    SCENE_TARGETS_SERVICE_MAJOR, SCENE_TARGETS_SERVICE_MINOR);
