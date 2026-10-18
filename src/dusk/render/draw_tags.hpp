#pragma once

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
};

inline constexpr size_t TagScopeCount = 3;

struct TagScopeDesc {
    const char* name;
    u32 categories;
    u32 flags;
    AuroraCameraRole role;
};

struct TagStats {
    u32 frame = 0;
    std::array<u32, TagScopeCount> scopes{};
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

TagStats last_tag_stats() noexcept;
std::string tag_count_text(const TagStats& stats);
std::vector<std::string> tag_report();
void draw_tags_self_test(SelfTestReport& report);

}  // namespace dusk::render
