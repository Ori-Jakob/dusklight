#include "dusk/render/draw_tags.hpp"

#include "dusk/render/hints.hpp"
#include "dusk/render/model_registry.hpp"
#include "dusk/render/render_bridge.hpp"

#include <aurora/gfx.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <numeric>
#include <string_view>
#include <utility>

namespace dusk::render {
namespace {

constexpr borealis::Log TagLog{"dusk::render"};
// aurora's scope stack holds 16 entries, so a deeper push would leave its pop unmatched there
constexpr u32 kMaxOpenScopes = 16;

constexpr std::array<TagScopeDesc, TagScopeCount> kRows{{
    {"sky", AURORA_CAT_SKY, 0, AURORA_CAMERA_SKY},
    {"screen_read", AURORA_CAT_SCREEN_READ | AURORA_CAT_NO_PBR, 0, AURORA_CAMERA_INHERIT},
    {"ui", AURORA_CAT_UI | AURORA_CAT_NO_CAPTURE | AURORA_CAT_NO_PBR, 0, AURORA_CAMERA_UI},
    {"grass", AURORA_CAT_FOLIAGE, AURORA_OBJ_CPU_DEFORMED, AURORA_CAMERA_INHERIT},
    {"flower", AURORA_CAT_FOLIAGE, AURORA_OBJ_CPU_DEFORMED, AURORA_CAMERA_INHERIT},
    {"line3d", AURORA_CAT_DYNAMIC, AURORA_OBJ_CPU_DEFORMED, AURORA_CAMERA_INHERIT},
    {"instanced", AURORA_CAT_DYNAMIC, 0, AURORA_CAMERA_INHERIT},
}};
static_assert(kRows.size() == TagScopeCount);

constexpr u32 kObjectFlags = AURORA_OBJ_CAPTURE_ONLY | AURORA_OBJ_TELEPORT |
                             AURORA_OBJ_CPU_DEFORMED | AURORA_OBJ_BAKED_VERTEX_LIGHT |
                             AURORA_OBJ_DOUBLE_SIDED | AURORA_OBJ_BILLBOARD | AURORA_OBJ_IMMOBILE;

struct TagScopeState {
    std::array<u32, TagScopeCount> open{};
    std::array<u32, TagScopeCount> refused{};
    TagStats current;
    TagStats last;
};

TagScopeState s_state;
bool s_warnedDepth = false;

size_t row_index(TagScope scope) noexcept {
    return static_cast<size_t>(scope);
}

u32 open_count(const TagScopeState& state) noexcept {
    return std::accumulate(state.open.begin(), state.open.end(), u32{0});
}

bool push(TagScopeState& state, TagScope scope, u32 frame) noexcept {
    const size_t i = row_index(scope);
    if (state.current.frame != frame) {
        state.last = state.current;
        state.current = TagStats{frame};
    }
    if (open_count(state) >= kMaxOpenScopes) {
        ++state.refused[i];
        return false;
    }
    ++state.open[i];
    ++state.current.scopes[i];
    return true;
}

// A refused push is always the innermost of its kind, so its end must not pop an outer scope
bool pop(TagScopeState& state, TagScope scope) noexcept {
    const size_t i = row_index(scope);
    if (state.refused[i] != 0) {
        --state.refused[i];
        return false;
    }
    if (state.open[i] == 0) {
        return false;
    }
    --state.open[i];
    return true;
}

// Same window and frame numbers as the registry log line, so both join to a trace on sourceFrame
void log_finished_frame(const TagStats& stats) {
    const u32 until = object_log_until();
    if (until != 0 && stats.frame <= until) {
        TagLog.info("render.objects log: frame {} scopes {}", stats.frame, tag_count_text(stats));
    }
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

std::string test_scope_table() {
    struct Expected {
        TagScope scope;
        std::string_view name;
        u32 categories;
        u32 flags;
        AuroraCameraRole role;
    };
    constexpr std::array kExpected{
        Expected{TagScope::Sky, "sky", AURORA_CAT_SKY, 0, AURORA_CAMERA_SKY},
        Expected{TagScope::ScreenRead, "screen_read", AURORA_CAT_SCREEN_READ | AURORA_CAT_NO_PBR, 0,
            AURORA_CAMERA_INHERIT},
        Expected{TagScope::Ui, "ui", AURORA_CAT_UI | AURORA_CAT_NO_CAPTURE | AURORA_CAT_NO_PBR, 0,
            AURORA_CAMERA_UI},
        Expected{TagScope::Grass, "grass", AURORA_CAT_FOLIAGE, AURORA_OBJ_CPU_DEFORMED,
            AURORA_CAMERA_INHERIT},
        Expected{TagScope::Flower, "flower", AURORA_CAT_FOLIAGE, AURORA_OBJ_CPU_DEFORMED,
            AURORA_CAMERA_INHERIT},
        Expected{TagScope::Line3d, "line3d", AURORA_CAT_DYNAMIC, AURORA_OBJ_CPU_DEFORMED,
            AURORA_CAMERA_INHERIT},
        Expected{
            TagScope::InstancedModel, "instanced", AURORA_CAT_DYNAMIC, 0, AURORA_CAMERA_INHERIT},
    };
    static_assert(kExpected.size() == TagScopeCount);
    std::string problems;
    for (size_t i = 0; i < TagScopeCount; ++i) {
        const std::string_view name = kRows[i].name == nullptr ? "" : kRows[i].name;
        if (name.empty()) {
            append_problem(problems, fmt::format("row {} has no name", i));
        }
        for (size_t j = 0; j < i; ++j) {
            if (name == kRows[j].name) {
                append_problem(problems, fmt::format("name {} is not unique", name));
            }
        }
        if (&tag_scope_desc(static_cast<TagScope>(i)) != &kRows[i]) {
            append_problem(problems, fmt::format("row {} is not found by its scope", i));
        }
        if ((kRows[i].categories & (AURORA_CAT_OVERLAY | AURORA_CAT_STATIC)) != 0) {
            append_problem(problems, fmt::format("row {} sets OVERLAY or STATIC", name));
        }
        if ((kRows[i].flags & ~kObjectFlags) != 0) {
            append_problem(problems, fmt::format("row {} has flags outside AURORA_OBJ_*", name));
        }
    }
    for (size_t i = 0; i < kExpected.size(); ++i) {
        const Expected& expected = kExpected[i];
        const TagScopeDesc& row = tag_scope_desc(expected.scope);
        if (static_cast<size_t>(expected.scope) != i || row.name != expected.name ||
            row.categories != expected.categories || row.flags != expected.flags ||
            row.role != expected.role)
        {
            append_problem(
                problems, fmt::format("row {} differs from the expected table", row.name));
        }
    }
    if (TagScopeCount != kRows.size()) {
        append_problem(problems, "TagScopeCount differs from the row table");
    }
    return problems;
}

std::string test_scope_balance() {
    std::string problems;
    const auto expect = [&problems](bool ok, std::string_view what) {
        if (!ok) {
            append_problem(problems, what);
        }
    };

    TagScopeState state;
    expect(push(state, TagScope::Sky, 1) && push(state, TagScope::ScreenRead, 1),
        "nested pushes refused");
    expect(pop(state, TagScope::ScreenRead) && pop(state, TagScope::Sky), "nested pops dropped");
    expect(open_count(state) == 0, "pairs leave scopes open");
    expect(!pop(state, TagScope::Ui) && open_count(state) == 0 && state.refused[2] == 0,
        "an end with nothing open is not ignored");
    expect(state.current.frame == 1 && state.current.scopes[0] == 1 &&
               state.current.scopes[1] == 1 && state.current.scopes[2] == 0,
        "frame counts wrong");
    expect(push(state, TagScope::Ui, 2) && state.last.frame == 1 && state.last.scopes[0] == 1 &&
               state.current.frame == 2 && state.current.scopes[2] == 1,
        "frame roll wrong");
    expect(pop(state, TagScope::Ui), "rolled scope not popped");

    TagScopeState full;
    bool accepted = true;
    for (u32 i = 0; i < kMaxOpenScopes; ++i) {
        accepted = push(full, static_cast<TagScope>(i % TagScopeCount), 3) && accepted;
    }
    expect(accepted && open_count(full) == kMaxOpenScopes, "16 open scopes refused");
    expect(!push(full, TagScope::Sky, 3) && open_count(full) == kMaxOpenScopes,
        "the 17th open scope is accepted");
    // The pushes cycle through the rows, so Sky took every TagScopeCount-th one
    constexpr u32 kSkyPushes =
        static_cast<u32>((kMaxOpenScopes + TagScopeCount - 1) / TagScopeCount);
    expect(full.current.scopes[0] == kSkyPushes, "a refused push is counted as sent");

    TagScopeState same;
    u32 pushes = 0;
    for (u32 i = 0; i <= kMaxOpenScopes; ++i) {
        pushes += push(same, TagScope::Sky, 4) ? 1 : 0;
    }
    u32 pops = pop(same, TagScope::Sky) ? 1 : 0;
    expect(pops == 0 && same.open[0] == kMaxOpenScopes && same.refused[0] == 0,
        "the end of a refused push pops an outer scope");
    for (u32 i = 0; i < kMaxOpenScopes; ++i) {
        pops += pop(same, TagScope::Sky) ? 1 : 0;
    }
    expect(pushes == kMaxOpenScopes && pops == kMaxOpenScopes && same.open[0] == 0 &&
               same.refused[0] == 0,
        fmt::format("17 pushes and ends of one kind give {} pushes, {} pops, {} open, {} refused",
            pushes, pops, same.open[0], same.refused[0]));
    return problems;
}

}  // namespace

const TagScopeDesc& tag_scope_desc(TagScope scope) noexcept {
    return kRows[row_index(scope)];
}

void begin_tag_scope(TagScope scope) noexcept {
    if (!hints_active()) {
        return;
    }
    const u32 frame = aurora::gfx::current_frame();
    if (s_state.current.frame != frame) {
        log_finished_frame(s_state.current);
    }
    const TagScopeDesc& row = tag_scope_desc(scope);
    if (!push(s_state, scope, frame)) {
        if (!std::exchange(s_warnedDepth, true)) {
            TagLog.warn("render tags: refused {} scope past {} open scopes in frame {}", row.name,
                kMaxOpenScopes, frame);
        }
        return;
    }
    GXAuroraPushScope(row.categories, row.flags, row.role);
}

void end_tag_scope(TagScope scope) noexcept {
    if (pop(s_state, scope)) {
        GXAuroraPopScope();
    }
}

u32 open_tag_scopes() noexcept {
    return open_count(s_state);
}

TagStats last_tag_stats() noexcept {
    return s_state.last;
}

std::string tag_count_text(const TagStats& stats) {
    std::string text;
    for (size_t i = 0; i < TagScopeCount; ++i) {
        if (!text.empty()) {
            text += ' ';
        }
        text += fmt::format("{} {}", kRows[i].name, stats.scopes[i]);
    }
    return text;
}

std::vector<std::string> tag_report() {
    const TagStats stats = last_tag_stats();
    return {fmt::format("render.objects: tag scopes frame {} {} open {}", stats.frame,
        tag_count_text(stats), open_tag_scopes())};
}

void draw_tags_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("tags.scope_table", test_scope_table());
    check("tags.scope_balance", test_scope_balance());
}

}  // namespace dusk::render
