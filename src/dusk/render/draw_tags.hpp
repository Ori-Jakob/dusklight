#pragma once

#include "dusk/render/model_registry.hpp"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/types.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace dusk::render {

struct SelfTestReport;

enum class TagScope : u8 {
    Sky,
    ScreenRead,
    Ui,
    Grass,
    Flower,
    Line3d,
    InstancedModel,
    ShadowImage,
    FakeShadow,
    Reflection,
};

inline constexpr size_t TagScopeCount = 10;

inline constexpr u32 kFakeShadowCategories =
    AURORA_CAT_FAKE_SHADOW | AURORA_CAT_DECAL | AURORA_CAT_NO_CAPTURE;

struct TagScopeDesc {
    const char* name;
    u32 categories;
    u32 flags;
    AuroraCameraRole role;
    bool suppressObjects = false;
};

struct TagStats {
    u32 frame = 0;
    std::array<u32, TagScopeCount> scopes{};
    u32 realShadows = 0;
    u32 simpleShadows = 0;
    u32 mirrorShapes = 0;
    u32 mirrorLeaks = 0;
};

const TagScopeDesc& tag_scope_desc(TagScope scope) noexcept;
void begin_tag_scope(TagScope scope) noexcept;
void end_tag_scope(TagScope scope) noexcept;
u32 open_tag_scopes() noexcept;

class TagScopeGuard {
public:
    [[nodiscard]] explicit TagScopeGuard(TagScope scope) noexcept : mScope(scope) {
        begin_tag_scope(scope);
    }
    ~TagScopeGuard() { end_tag_scope(mScope); }
    TagScopeGuard(const TagScopeGuard&) = delete;
    TagScopeGuard& operator=(const TagScopeGuard&) = delete;

private:
    TagScope mScope;
};

class ShadowEntryObject {
public:
    [[nodiscard]] ShadowEntryObject(
        ObjectKind kind, const J3DModel* caster, int slot, bool draws) noexcept;
    ~ShadowEntryObject();
    ShadowEntryObject(const ShadowEntryObject&) = delete;
    ShadowEntryObject& operator=(const ShadowEntryObject&) = delete;

private:
    bool mOpen;
};

class MirrorDrawCheck {
public:
    [[nodiscard]] MirrorDrawCheck() noexcept;
    ~MirrorDrawCheck();
    MirrorDrawCheck(const MirrorDrawCheck&) = delete;
    MirrorDrawCheck& operator=(const MirrorDrawCheck&) = delete;

private:
    RegistryStats mStart;
};

TagStats last_tag_stats() noexcept;
std::string tag_count_text(const TagStats& stats);
std::vector<std::string> tag_report();
void draw_tags_self_test(SelfTestReport& report);

}  // namespace dusk::render
