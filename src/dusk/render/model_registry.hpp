#pragma once

#include "dusk/render/material_tags.hpp"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/types.h>

#include <string>
#include <vector>

class J3DModel;
class J3DModelData;
class J3DShapePacket;
class JKRHeap;

namespace dusk::render {

struct SelfTestReport;

enum class ObjectKind : u8 {
    None = 0,
    J3dModel = 1,
    RealShadow = 2,
    SimpleShadow = 3,
};

constexpr u64 make_object_id(ObjectKind kind, u32 value) noexcept {
    return (static_cast<u64>(kind) << 32) | value;
}

constexpr bool is_model_resource(u32 nodeType) noexcept {
    return nodeType == 'BMDP' || nodeType == 'BMDR' || nodeType == 'BMDV' || nodeType == 'BMDE' ||
           nodeType == 'BMWR' || nodeType == 'BMWE' || nodeType == 'BMDG' || nodeType == 'BMDA';
}

struct ShapeFacts {
    bool skinPosCpu = false;
    bool deformData = false;
    bool localPositions = false;
    bool billboard = false;
    bool teleport = false;
};

struct ShapeInputs {
    u32 serial = 0;
    u16 shapeIndex = 0;
    u32 generation = 0;
    u32 modelCategories = 0;
    u32 modelFlags = 0;
    const MaterialTag* material = nullptr;
    bool pendingTags = false;
    ShapeFacts facts;
};

struct ShapeObjectHint {
    u64 id = 0;
    u32 subId = 0;
    u32 generation = 0;
    u32 categories = 0;
    u32 flags = 0;
};

struct RegistryStats {
    u32 frame = 0;
    u32 objects = 0;
    u32 staticObjects = 0;
    u32 nonRoomStatics = 0;
    u32 suppressedShapes = 0;
    u32 nested = 0;
    u32 cpuDeformed = 0;
    u32 billboards = 0;
    u32 declaredArrays = 0;
    u32 releasedArrays = 0;
};

void on_model_initialized(const J3DModel* model) noexcept;
void on_model_data_created(const J3DModelData* data) noexcept;
void on_model_loaded(const J3DModelData* data, const char* archive, const JKRHeap* heap) noexcept;
void on_resource_heap_destroyed(const JKRHeap* heap) noexcept;
void declare_room_model(const J3DModel* model, int roomNo, int part) noexcept;
void declare_model(const J3DModel* model, u32 categories, u32 flags) noexcept;
void mark_model_teleport(const J3DModel* model) noexcept;
u32 model_serial(const J3DModel* model) noexcept;
u32 data_generation(const J3DModelData* data) noexcept;

ShapeObjectHint compose_shape_object(const ShapeInputs& in) noexcept;

void push_object_suppression() noexcept;
void pop_object_suppression() noexcept;
void note_object_open() noexcept;
void note_object_close() noexcept;

bool begin_shape(const J3DShapePacket* packet) noexcept;

class ShapeObject {
public:
    [[nodiscard]] explicit ShapeObject(const J3DShapePacket* packet) noexcept
        : mOpen(begin_shape(packet)) {}
    ~ShapeObject() {
        if (mOpen) {
            GXAuroraEndObject();
            note_object_close();
        }
    }
    ShapeObject(const ShapeObject&) = delete;
    ShapeObject& operator=(const ShapeObject&) = delete;

private:
    bool mOpen;
};

RegistryStats current_registry_stats() noexcept;
RegistryStats last_registry_stats() noexcept;
void set_object_log(u32 frames) noexcept;
u32 object_log_until() noexcept;
std::vector<std::string> registry_report();
std::vector<std::string> material_tag_report();
void registry_self_test(SelfTestReport& report);

}  // namespace dusk::render
