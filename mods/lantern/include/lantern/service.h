#pragma once

#include <mods/api.h>

#define LANTERN_SERVICE_ID "dev.twilitrealm.lantern"
#define LANTERN_SERVICE_MAJOR 1u
#define LANTERN_SERVICE_MINOR 0u

typedef uint64_t LanternModuleHandle;
typedef uint64_t LanternPeerId;

typedef enum LanternDelivery {
    LANTERN_RELIABLE_ORDERED = 0,
    LANTERN_UNRELIABLE_LATEST = 1,
} LanternDelivery;

typedef enum LanternConnectionState {
    LANTERN_DISCONNECTED = 0,
    LANTERN_CONNECTING = 1,
    LANTERN_NEGOTIATING = 2,
    LANTERN_CONNECTED = 3,
    LANTERN_RECONNECTING = 4,
} LanternConnectionState;

typedef enum LanternModuleFlags {
    LANTERN_MODULE_OPTIONAL = 0,
    LANTERN_MODULE_REQUIRED = 1u << 0u,
    LANTERN_MODULE_CLIENT_ONLY = 1u << 1u,
    LANTERN_MODULE_COSMETIC = 1u << 2u,
} LanternModuleFlags;

typedef enum LanternSessionEvent {
    LANTERN_SESSION_CONNECTING = 0,
    LANTERN_SESSION_CONNECTED = 1,
    LANTERN_SESSION_DISCONNECTED = 2,
    LANTERN_SESSION_REJECTED = 3,
    LANTERN_SESSION_COMPATIBILITY_CHANGED = 4,
} LanternSessionEvent;

typedef enum LanternPeerEvent {
    LANTERN_PEER_JOINED = 0,
    LANTERN_PEER_LEFT = 1,
    LANTERN_PEER_MANIFEST_CHANGED = 2,
} LanternPeerEvent;

typedef struct LanternPeerInfo {
    uint32_t struct_size;
    LanternPeerId peer_id;
    const char* display_name;
    uint32_t color_rgb;
    uint32_t compatible_module_count;
} LanternPeerInfo;

#define LANTERN_PEER_INFO_INIT {sizeof(LanternPeerInfo), 0u, NULL, 0u, 0u}

typedef struct LanternMessage {
    uint32_t struct_size;
    LanternPeerId sender_peer_id;
    LanternDelivery delivery;
    uint32_t sequence;
    const void* data;
    size_t size;
} LanternMessage;

typedef void (*LanternSessionFn)(
    ModContext* owner, LanternSessionEvent event, const char* detail, void* user_data);
typedef void (*LanternPeerFn)(
    ModContext* owner, LanternPeerEvent event, const LanternPeerInfo* peer, void* user_data);
typedef void (*LanternManifestFn)(
    ModContext* owner, LanternPeerId peer_id, const char* compatibility_report, void* user_data);
typedef void (*LanternMessageFn)(
    ModContext* owner, LanternModuleHandle module, const LanternMessage* message, void* user_data);

typedef struct LanternModuleDesc {
    uint32_t struct_size;
    const char* module_id;
    const char* display_version;
    uint16_t protocol_major;
    uint16_t protocol_minor;
    uint16_t minimum_peer_minor;
    uint16_t reserved;
    uint32_t flags;
    const char* compatibility_tag;
    const char* distribution_id;
    LanternSessionFn on_session;
    LanternPeerFn on_peer;
    LanternManifestFn on_manifest;
    LanternMessageFn on_message;
    void* user_data;
} LanternModuleDesc;

#define LANTERN_MODULE_DESC_INIT                                                                   \
    {sizeof(LanternModuleDesc), NULL, NULL, 1u, 0u, 0u, 0u, LANTERN_MODULE_OPTIONAL, NULL, NULL,   \
        NULL, NULL, NULL, NULL, NULL}

typedef struct LanternSendDesc {
    uint32_t struct_size;
    LanternModuleHandle module;
    LanternPeerId target_peer_id; /* 0 broadcasts to every compatible peer. */
    LanternDelivery delivery;
    const void* data;
    size_t size;
} LanternSendDesc;

#define LANTERN_SEND_DESC_INIT {sizeof(LanternSendDesc), 0u, 0u, LANTERN_RELIABLE_ORDERED, NULL, 0u}

typedef struct LanternService {
    ServiceHeader header;

    /* Registration ownership is inferred from owner. All callbacks run on the game thread. */
    ModResult (*register_module)(
        ModContext* owner, const LanternModuleDesc* desc, LanternModuleHandle* out_handle);
    ModResult (*unregister_module)(ModContext* owner, LanternModuleHandle handle);
    ModResult (*send)(ModContext* owner, const LanternSendDesc* desc);

    LanternConnectionState (*connection_state)(ModContext* owner);
    LanternPeerId (*self_peer_id)(ModContext* owner);
    size_t (*peer_count)(ModContext* owner);
    ModResult (*peer_at)(ModContext* owner, size_t index, LanternPeerInfo* out_peer);
    const char* (*room_code)(ModContext* owner);
    const char* (*compatibility_report)(ModContext* owner);
} LanternService;

#ifdef __cplusplus
#include <mods/service.hpp>

template <>
struct mods::ServiceTraits<LanternService> {
    static constexpr const char* id = LANTERN_SERVICE_ID;
    static constexpr uint16_t major_version = LANTERN_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = LANTERN_SERVICE_MINOR;
};
#endif
