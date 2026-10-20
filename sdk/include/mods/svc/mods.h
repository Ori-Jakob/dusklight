#pragma once

#include <mods/api.h>
#include <mods/svc/texture.h>

#ifdef __cplusplus
#include <mods/service.hpp>
#endif

/*
 * What the player has installed: the active mods with their mod.json, and the files and textures
 * each one overrides. For mods that build on other mods' content (e.g. showing a player's
 * installed looks to other players). Native mods only.
 *
 * list_mods, list_overrides, generation, the var getters, watch and unwatch run on the game
 * thread. hash_override, read_override, read_base_disc_file and free_buffer may run on any thread,
 * so large reads can stay off the game thread.
 */

#define MODS_SERVICE_ID DUSKLIGHT_SERVICE_ID_PREFIX "mods"
#define MODS_SERVICE_MAJOR 1u
#define MODS_SERVICE_MINOR 0u

typedef struct ModInfo {
    uint32_t struct_size;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    /* The bundle's mod.json as loaded, unparsed (keys the loader ignores included). */
    const char* manifest_json;
    size_t manifest_len;
    /* Position in the load order; a later mod wins file and texture conflicts. */
    uint32_t load_order;
    bool has_native;
    bool has_script;
} ModInfo;

typedef enum ModOverrideKind {
    /* A whole disc file, e.g. "/res/Object/Kmdl.arc". */
    MOD_OVR_DISC_FILE = 0,
    /* One entry inside an archive, e.g. "/res/Object/Kmdl/kmdl/bmdr/al.bmd". */
    MOD_OVR_ARC_ENTRY = 1,
    /* A texture replacement. */
    MOD_OVR_TEXTURE = 2,
} ModOverrideKind;

typedef struct ModOverride {
    uint32_t struct_size;
    /* Stable for as long as the override exists; never reused for another one. */
    uint64_t id;
    ModOverrideKind kind;
    /* Registered through OverlayService or TextureService (false: overlay/ or textures/). */
    bool runtime;
    /* No other active override of the same disc path (case-insensitive) or texture key beats it. */
    bool wins;
    /* DISC_FILE, ARC_ENTRY: the overridden path as registered. */
    const char* disc_path;
    /* ARC_ENTRY: the archive's disc path, e.g. "/res/Object/Kmdl.arc". */
    const char* archive_path;
    /* OverlayService::add_base_alias: the disc file served, read from the disc itself. */
    const char* alias_of;
    /* The file in the owner's bundle behind it ("overlay/...", "textures/..."), if any. */
    const char* bundle_path;
    /* Bytes that read_override returns. */
    uint64_t size;
    /* TEXTURE: the key it replaces. */
    TextureKey texture_key;
    /* TEXTURE: raw GX texels described by the texture_* fields (TextureService::register_data);
     * false for an encoded .dds/.png. */
    bool texture_raw;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_mip_count;
    uint32_t texture_gx_format;
} ModOverride;

/* Bytes allocated by the service. Return every successful read with free_buffer. */
typedef struct ModBuffer {
    uint32_t struct_size;
    void* data;
    size_t size;
} ModBuffer;

#define MOD_BUFFER_INIT {sizeof(ModBuffer), NULL, 0u}

typedef enum ModTextureSource {
    MOD_TEXTURE_SOURCE_NONE = 0,
    /* A replacement keyed by the texel pointer (TEXTURE_KEY_POINTER) applies. */
    MOD_TEXTURE_SOURCE_POINTER = 1,
    /* A replacement keyed by content (TEXTURE_KEY_SOURCE, wildcards included) applies. */
    MOD_TEXTURE_SOURCE_SOURCE = 2,
} ModTextureSource;

typedef struct ModOverlayStats {
    uint32_t struct_size;
    /* Archive entry overlays currently copied into memory by mounted archives. */
    uint64_t live_entry_buffers;
    /* Archive entry overlay copies made since boot. */
    uint64_t total_entry_buffers;
    /* Overlay sets applied since boot; each one rebuilds the disc's file table. */
    uint64_t overlay_syncs;
} ModOverlayStats;

#define MOD_OVERLAY_STATS_INIT {sizeof(ModOverlayStats), 0u, 0u, 0u}

/* Return false to stop the listing. The pointed-to data is valid only during the call. */
typedef bool (*ModInfoFn)(void* user_data, const ModInfo* info);
typedef bool (*ModOverrideFn)(void* user_data, const ModOverride* override_info);
/* On the game thread, at the end of a frame in which the generation changed. */
typedef void (*ModsChangedFn)(ModContext* ctx, uint64_t generation, void* user_data);

typedef struct ModsService {
    ServiceHeader header;

    /* Changes whenever the active mods or any of their overrides change. */
    uint64_t (*generation)(ModContext* ctx);

    /* Every active mod, the caller included, in load order. */
    ModResult (*list_mods)(ModContext* ctx, ModInfoFn fn, void* user_data);

    /* The overrides of an active mod: overlay/ files, textures/ files, then runtime registrations.
     * MOD_UNAVAILABLE if no active mod has that id. */
    ModResult (*list_overrides)(
        ModContext* ctx, const char* mod_id, ModOverrideFn fn, void* user_data);

    /* SHA-256 of what read_override returns. Computed on first request and cached until the
     * owner reloads. MOD_UNAVAILABLE if the override is gone. */
    ModResult (*hash_override)(
        ModContext* ctx, const char* mod_id, uint64_t id, uint8_t out_sha256[32]);

    /* The override's bytes: the overlay file, buffer or aliased disc file, or the texture file
     * (.dds/.png) or raw texels. */
    ModResult (*read_override)(ModContext* ctx, const char* mod_id, uint64_t id, ModBuffer* out);

    /* A file as it is on the disc, ignoring every overlay. MOD_UNAVAILABLE if the disc has no
     * file at disc_path. */
    ModResult (*read_base_disc_file)(ModContext* ctx, const char* disc_path, ModBuffer* out);

    /*
     * Another mod's settings, read-only and only those Dusklight lists as shareable (for now the
     * cosmetics mod's colors); MOD_UNAVAILABLE for any other var or when the mod is not active.
     * get_mod_var_string follows ConfigService::get_string: out_length (optional) receives the
     * length without the NUL, a NULL buffer with size 0 queries it, and a buffer that is too small
     * fails with MOD_INVALID_ARGUMENT. A type mismatch is MOD_INVALID_ARGUMENT.
     */
    ModResult (*get_mod_var_string)(ModContext* ctx, const char* mod_id, const char* name,
        char* buffer, size_t buffer_size, size_t* out_length);
    ModResult (*get_mod_var_int)(
        ModContext* ctx, const char* mod_id, const char* name, int64_t* out_value);

    /* Call fn when the generation changes. Removed automatically when the caller goes away. */
    ModResult (*watch)(ModContext* ctx, ModsChangedFn fn, void* user_data, uint64_t* out_handle);
    ModResult (*unwatch)(ModContext* ctx, uint64_t handle);

    /* Release a buffer from read_override or read_base_disc_file and reset it to empty. */
    void (*free_buffer)(ModContext* ctx, ModBuffer* buffer);

    /*
     * For tests: which replacement the renderer would apply to a texture. tex_obj is a
     * GXTexObj*, tlut_obj a GXTlutObj* for palette formats or NULL.
     */
    ModResult (*debug_texture_source)(
        ModContext* ctx, const void* tex_obj, const void* tlut_obj, ModTextureSource* out);

    /* For tests: archive entry overlay memory and overlay set rebuilds. */
    ModResult (*debug_overlay_stats)(ModContext* ctx, ModOverlayStats* out);

    /* The loaded disc: game id ("GZ2E01") and revision. */
    const char* game_id;
    uint8_t disc_revision;
} ModsService;

MOD_DECLARE_SERVICE(ModsService, svc_mods, MODS_SERVICE_ID, MODS_SERVICE_MAJOR, MODS_SERVICE_MINOR);
