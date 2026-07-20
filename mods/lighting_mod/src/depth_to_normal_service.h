// Depth to Normal shared-service consumer contract.
// Kept ABI-identical to automata-rtx/dusklight-mods Graphics Hub v1.0. The service is optional:
// lighting_mod reconstructs its own RGBA16F normals when no provider is installed.
#ifndef DEPTH_TO_NORMAL_SERVICE_H
#define DEPTH_TO_NORMAL_SERVICE_H

#include "mods/api.h"

#include <webgpu/webgpu.h>

#define DEPTH_TO_NORMAL_SERVICE_ID "dev.automata.depth_to_normal"
#define DEPTH_TO_NORMAL_SERVICE_MAJOR 1u
#define DEPTH_TO_NORMAL_SERVICE_MINOR 0u

typedef struct DepthToNormalFrame {
    uint32_t struct_size;
    WGPUTextureView normal; /* rgba32float: xyz = world-space geometric normal (unit,
                             * camera-facing), w = raw reversed-Z depth. Frame-valid. */
    uint32_t width;
    uint32_t height;
} DepthToNormalFrame;

#define DEPTH_TO_NORMAL_FRAME_INIT {sizeof(DepthToNormalFrame), NULL, 0u, 0u}

typedef struct DepthToNormalService {
    ServiceHeader header;
    ModResult (*get_frame)(ModContext* ctx, DepthToNormalFrame* out);
} DepthToNormalService;

#ifdef __cplusplus
#include "mods/service.hpp"
template <>
struct mods::ServiceTraits<DepthToNormalService> {
    static constexpr const char* id = DEPTH_TO_NORMAL_SERVICE_ID;
    static constexpr uint16_t major_version = DEPTH_TO_NORMAL_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = DEPTH_TO_NORMAL_SERVICE_MINOR;
};
#endif

#endif  // DEPTH_TO_NORMAL_SERVICE_H
