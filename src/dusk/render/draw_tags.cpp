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
    {"shadow_image", 0, 0, AURORA_CAMERA_SHADOW, true},
    {"fake_shadow", kFakeShadowCategories, 0, AURORA_CAMERA_INHERIT},
    {"reflection", 0, 0, AURORA_CAMERA_REFLECTION, true},
}};
static_assert(kRows.size() == TagScopeCount);
static_assert((kFakeShadowCategories & AURORA_CAT_OVERLAY) == 0);

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

void roll(TagScopeState& state, u32 frame) noexcept {
    if (state.current.frame != frame) {
        state.last = state.current;
        state.current = TagStats{frame};
    }
}

bool push(TagScopeState& state, TagScope scope, u32 frame) noexcept {
    const size_t i = row_index(scope);
    roll(state, frame);
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
        TagLog.info("render.objects log: frame {} scopes {} real {} simple {} mirror {} leaks {}",
            stats.frame, tag_count_text(stats), stats.realShadows, stats.simpleShadows,
            stats.mirrorShapes, stats.mirrorLeaks);
    }
}

// Counts made outside a scope push must still land in the frame that drew them
TagStats& live_frame() noexcept {
    const u32 frame = aurora::gfx::current_frame();
    if (s_state.current.frame != frame) {
        log_finished_frame(s_state.current);
        roll(s_state, frame);
    }
    return s_state.current;
}

// Unknown casters fall back to their slot, so their entries still get distinct ids
u32 shadow_entry_value(u32 serial, int slot) noexcept {
    return serial != 0 ? serial : static_cast<u32>(slot);
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

std::string test_scope_table() {
    constexpr u32 kObjectFlags = AURORA_OBJ_CAPTURE_ONLY | AURORA_OBJ_TELEPORT |
                                 AURORA_OBJ_CPU_DEFORMED | AURORA_OBJ_BAKED_VERTEX_LIGHT |
                                 AURORA_OBJ_DOUBLE_SIDED | AURORA_OBJ_BILLBOARD |
                                 AURORA_OBJ_IMMOBILE;
    struct Expected {
        TagScope scope;
        std::string_view name;
        u32 categories;
        u32 flags;
        AuroraCameraRole role;
        bool suppressObjects = false;
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
        Expected{TagScope::ShadowImage, "shadow_image", 0, 0, AURORA_CAMERA_SHADOW, true},
        Expected{TagScope::FakeShadow, "fake_shadow",
            AURORA_CAT_FAKE_SHADOW | AURORA_CAT_DECAL | AURORA_CAT_NO_CAPTURE, 0,
            AURORA_CAMERA_INHERIT},
        Expected{TagScope::Reflection, "reflection", 0, 0, AURORA_CAMERA_REFLECTION, true},
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
            row.role != expected.role || row.suppressObjects != expected.suppressObjects)
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

std::string test_fake_shadow_categories() {
    std::string problems;
    if (kFakeShadowCategories !=
        (AURORA_CAT_FAKE_SHADOW | AURORA_CAT_DECAL | AURORA_CAT_NO_CAPTURE))
    {
        append_problem(problems, "kFakeShadowCategories is not FAKE_SHADOW|DECAL|NO_CAPTURE");
    }
    if ((kFakeShadowCategories & AURORA_CAT_OVERLAY) != 0) {
        append_problem(problems, "kFakeShadowCategories sets OVERLAY");
    }
    if (tag_scope_desc(TagScope::FakeShadow).categories != kFakeShadowCategories) {
        append_problem(problems, "the fake_shadow row does not use kFakeShadowCategories");
    }
    return problems;
}

std::string test_redraw_roles() {
    std::string problems;
    const auto expect_row = [&problems](TagScope scope, AuroraCameraRole role, bool suppress) {
        const TagScopeDesc& row = tag_scope_desc(scope);
        if (row.role != role || row.suppressObjects != suppress) {
            append_problem(problems, fmt::format("row {} has role {} and suppression {}", row.name,
                                         static_cast<int>(row.role), row.suppressObjects));
        }
    };
    expect_row(TagScope::ShadowImage, AURORA_CAMERA_SHADOW, true);
    expect_row(TagScope::FakeShadow, AURORA_CAMERA_INHERIT, false);
    expect_row(TagScope::Reflection, AURORA_CAMERA_REFLECTION, true);
    for (size_t i = 0; i < row_index(TagScope::ShadowImage); ++i) {
        if (kRows[i].suppressObjects) {
            append_problem(problems, fmt::format("row {} suppresses objects", kRows[i].name));
        }
    }
    return problems;
}

std::string test_shadow_entry_ids() {
    std::string problems;
    for (const u32 value : {0u, 1u, 127u, 0xFFFF'FFFFu}) {
        const u64 model = make_object_id(ObjectKind::J3dModel, value);
        const u64 real = make_object_id(ObjectKind::RealShadow, value);
        const u64 simple = make_object_id(ObjectKind::SimpleShadow, value);
        if (real == model || simple == model || real == simple) {
            append_problem(problems, fmt::format("ids for value {} collide", value));
        }
    }
    if (shadow_entry_value(42, 3) != 42 || shadow_entry_value(0, 3) != 3) {
        append_problem(problems, "an unknown caster does not fall back to its slot");
    }
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
    const u32 frame = live_frame().frame;
    const TagScopeDesc& row = tag_scope_desc(scope);
    if (!push(s_state, scope, frame)) {
        if (!std::exchange(s_warnedDepth, true)) {
            TagLog.warn("render tags: refused {} scope past {} open scopes in frame {}", row.name,
                kMaxOpenScopes, frame);
        }
        return;
    }
    GXAuroraPushScope(row.categories, row.flags, row.role);
    if (row.suppressObjects) {
        push_object_suppression();
    }
}

void end_tag_scope(TagScope scope) noexcept {
    if (!pop(s_state, scope)) {
        return;
    }
    if (tag_scope_desc(scope).suppressObjects) {
        pop_object_suppression();
    }
    GXAuroraPopScope();
}

u32 open_tag_scopes() noexcept {
    return open_count(s_state);
}

ShadowEntryObject::ShadowEntryObject(
    ObjectKind kind, const J3DModel* caster, int slot, bool draws) noexcept
    : mOpen(draws && hints_active()) {
    if (!mOpen) {
        return;
    }
    const u32 serial = caster != nullptr ? model_serial(caster) : 0;
    GXAuroraBeginObject(
        make_object_id(kind, shadow_entry_value(serial, slot)), 0, 0, kFakeShadowCategories, 0);
    note_object_open();
    TagStats& stats = live_frame();
    if (kind == ObjectKind::RealShadow) {
        ++stats.realShadows;
    } else {
        ++stats.simpleShadows;
    }
}

ShadowEntryObject::~ShadowEntryObject() {
    if (mOpen) {
        GXAuroraEndObject();
        note_object_close();
    }
}

MirrorDrawCheck::MirrorDrawCheck() noexcept {
    if (hints_active()) {
        mStart = current_registry_stats();
    }
}

// Any object the mirror packet emits has escaped the suppressing Reflection scope
MirrorDrawCheck::~MirrorDrawCheck() {
    if (!hints_active()) {
        return;
    }
    const RegistryStats now = current_registry_stats();
    TagStats& stats = live_frame();
    stats.mirrorLeaks += now.objects - mStart.objects;
    stats.mirrorShapes += now.suppressedShapes - mStart.suppressedShapes;
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
    return {
        fmt::format("render.objects: tag scopes frame {} {} open {}", stats.frame,
            tag_count_text(stats), open_tag_scopes()),
        fmt::format("render.objects: fake shadows real {} simple {}", stats.realShadows,
            stats.simpleShadows),
        fmt::format(
            "render.objects: mirror shapes {} leaks {}", stats.mirrorShapes, stats.mirrorLeaks),
    };
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
    check("tags.fake_shadow_categories", test_fake_shadow_categories());
    check("tags.redraw_roles", test_redraw_roles());
    check("tags.shadow_entry_ids", test_shadow_entry_ids());
}

}  // namespace dusk::render
