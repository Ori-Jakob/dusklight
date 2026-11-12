#include "dusk/render/fake_shadows.hpp"

#include "dusk/render/render_bridge.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_tev_str.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor.h"

#include <aurora/gfx.hpp>
#include <aurora/render.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace dusk::render::fake_shadows {
namespace {

static_assert(offsetof(dKy_tevstr_c, dusk_shadowSrc) == 0x386 && sizeof(dKy_tevstr_c) == 0x388);

using aurora::render::ShadowSource;
static_assert(static_cast<u8>(Src::Unknown) == static_cast<u8>(ShadowSource::Unknown));
static_assert(static_cast<u8>(Src::Vertical) == static_cast<u8>(ShadowSource::Vertical));
static_assert(static_cast<u8>(Src::SunMoon) == static_cast<u8>(ShadowSource::SunMoon));
static_assert(static_cast<u8>(Src::RoomKey) == static_cast<u8>(ShadowSource::RoomKey));
static_assert(static_cast<u8>(Src::PointLight) == static_cast<u8>(ShadowSource::PointLight));
static_assert(static_cast<u8>(Src::RoomLight) == static_cast<u8>(ShadowSource::RoomLight));
static_assert(static_cast<u8>(Src::Lantern) == static_cast<u8>(ShadowSource::Lantern));
static_assert(static_cast<u8>(Src::EffectLight) == static_cast<u8>(ShadowSource::EffectLight));

constexpr borealis::Log FakeLog{"dusk::render"};
constexpr u32 kTableSlots = 1024;
constexpr u32 kNoFrame = UINT32_MAX;
constexpr std::string_view kUsage = "Usage: render.fakeshadows [log <n>]";

constexpr std::array<std::string_view, SrcCount> kSourceNames{"Unknown", "Vertical", "SunMoon",
    "RoomKey", "PointLight", "RoomLight", "Lantern", "EffectLight"};

struct TagSlot {
    const void* key = nullptr;
    std::array<f32, 3> pos{};
    u32 frame = kNoFrame;
};

class TagTable {
public:
    bool note(const void* key, const Vec& pos, u32 frame) noexcept {
        const u32 start = slot_hash(key);
        for (u32 i = 0; i < kTableSlots; ++i) {
            TagSlot& slot = m_slots[(start + i) & (kTableSlots - 1)];
            if (slot.frame != frame || slot.key == key) {
                slot = {key, {pos.x, pos.y, pos.z}, frame};
                return true;
            }
        }
        ++m_overflow;
        return false;
    }

    bool unchanged(const void* key, const Vec& pos, u32 frame) const noexcept {
        const u32 start = slot_hash(key);
        for (u32 i = 0; i < kTableSlots; ++i) {
            const TagSlot& slot = m_slots[(start + i) & (kTableSlots - 1)];
            if (slot.frame != frame) {
                return false;
            }
            if (slot.key == key) {
                const std::array<f32, 3> now{pos.x, pos.y, pos.z};
                return std::memcmp(now.data(), slot.pos.data(), sizeof(now)) == 0;
            }
        }
        return false;
    }

    u64 overflow() const noexcept { return m_overflow; }

private:
    static u32 slot_hash(const void* key) noexcept {
        u64 x = reinterpret_cast<uintptr_t>(key) + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return static_cast<u32>((x ^ (x >> 31)) & (kTableSlots - 1));
    }

    std::array<TagSlot, kTableSlots> m_slots{};
    u64 m_overflow = 0;
};

struct TagCounts {
    u32 frame = kNoFrame;
    u32 tagged = 0;
    u32 overflow = 0;
    std::array<u32, SrcCount> sources{};
    Src link = Src::Unknown;
    bool linkTagged = false;
};

struct LiveTags {
    TagTable table;
    Src baseLight = Src::Unknown;
    Src flame = Src::Unknown;
    TagCounts current;
    TagCounts last;
    u32 logUntil = 0;
};

LiveTags& live() noexcept {
    static LiveTags& tags = *new LiveTags;
    return tags;
}

Src checked(u8 value) noexcept {
    return value < SrcCount ? static_cast<Src>(value) : Src::Unknown;
}

Src resolve(const TagTable& table, const dKy_tevstr_c* tevstr, u32 frame) noexcept {
    if (tevstr == nullptr || !table.unchanged(tevstr, tevstr->mLightPosWorld, frame)) {
        return Src::Unknown;
    }
    return checked(tevstr->dusk_shadowSrc);
}

u32 stage_type() {
    stage_stag_info_class* info = dComIfGp_getStage()->getStagInfo();
    return info != nullptr ? dStage_stagInfo_GetSTType(info) : ST_FIELD;
}

std::string counts_text(const TagCounts& counts) {
    std::string text;
    for (size_t i = 0; i < SrcCount; ++i) {
        text += fmt::format(" {} {}", kSourceNames[i], counts.sources[i]);
    }
    return text;
}

void log_tags(const TagCounts& counts) {
    const char* stage = dComIfGp_getStartStageName();
    FakeLog.info("render.fakeshadows log: frame {} stage {} room {} interior {} tagged {} "
                 "overflow {} link {}{}",
        counts.frame, stage != nullptr ? stage : "", dComIfGp_roomControl_getStayNo(),
        stage_type() != ST_FIELD ? 1 : 0, counts.tagged, counts.overflow,
        counts.linkTagged ? kSourceNames[static_cast<size_t>(counts.link)] : "none",
        counts_text(counts));
}

void roll(LiveTags& tags, u32 frame) {
    if (tags.current.frame == frame) {
        return;
    }
    if (tags.current.frame != kNoFrame && tags.logUntil != 0 && tags.current.frame <= tags.logUntil)
    {
        log_tags(tags.current);
    }
    tags.last = tags.current;
    tags.current = TagCounts{.frame = frame};
}

bool parse_count(std::string_view text, u32& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

std::unique_ptr<dKy_tevstr_c> tagged_tevstr(TagTable& table, Src src, u32 frame) {
    auto tevstr = std::make_unique<dKy_tevstr_c>();
    std::memset(tevstr.get(), 0, sizeof(dKy_tevstr_c));
    tevstr->mLightPosWorld.set(100.0f, 2000.0f, -300.0f);
    tevstr->dusk_shadowSrc = static_cast<u8>(src);
    table.note(tevstr.get(), tevstr->mLightPosWorld, frame);
    return tevstr;
}

std::string test_tevstr_layout() {
    auto tevstr = std::make_unique<dKy_tevstr_c>();
    std::memset(tevstr.get(), 0xFF, sizeof(dKy_tevstr_c));
    dKy_tevstr_init(tevstr.get(), 0, 0);
    std::string problems;
    if (tevstr->dusk_shadowSrc != static_cast<u8>(Src::Unknown)) {
        append_problem(problems, fmt::format("init leaves {}", tevstr->dusk_shadowSrc));
    }
    if (offsetof(dKy_tevstr_c, dusk_shadowSrc) != 0x386 || sizeof(dKy_tevstr_c) != 0x388) {
        append_problem(problems, "layout moved");
    }
    return problems;
}

std::string test_source_mirror() {
    std::string problems;
    for (u8 i = 0; i < SrcCount; ++i) {
        if (static_cast<u8>(static_cast<ShadowSource>(i)) != static_cast<u8>(static_cast<Src>(i)) ||
            checked(i) != static_cast<Src>(i))
        {
            append_problem(problems, fmt::format("source {} differs", i));
        }
    }
    if (checked(static_cast<u8>(SrcCount)) != Src::Unknown) {
        append_problem(problems, "out of range is not Unknown");
    }
    return problems;
}

std::string test_override_is_unknown() {
    TagTable table;
    auto tevstr = tagged_tevstr(table, Src::SunMoon, 10);
    std::string problems;
    if (resolve(table, tevstr.get(), 10) != Src::SunMoon) {
        append_problem(problems, "unmoved tag lost");
    }
    tevstr->mLightPosWorld.y += 1000.0f;
    tevstr->dusk_shadowSrc = static_cast<u8>(Src::Vertical);
    if (resolve(table, tevstr.get(), 10) != Src::Unknown) {
        append_problem(problems, "moved light kept its tag");
    }
    if (resolve(table, nullptr, 10) != Src::Unknown) {
        append_problem(problems, "null is not Unknown");
    }
    return problems;
}

std::string test_copied_tevstr_is_unknown() {
    TagTable table;
    auto tevstr = tagged_tevstr(table, Src::SunMoon, 10);
    auto copy = std::make_unique<dKy_tevstr_c>();
    std::memcpy(copy.get(), tevstr.get(), sizeof(dKy_tevstr_c));
    std::string problems;
    if (copy->dusk_shadowSrc != static_cast<u8>(Src::SunMoon)) {
        append_problem(problems, "copy lost the field");
    }
    if (resolve(table, copy.get(), 10) != Src::Unknown) {
        append_problem(problems, "copy kept the tag");
    }
    return problems;
}

std::string test_stale_stamp_is_unknown() {
    TagTable table;
    auto tevstr = tagged_tevstr(table, Src::PointLight, 10);
    std::string problems;
    if (resolve(table, tevstr.get(), 11) != Src::Unknown) {
        append_problem(problems, "last frame's tag kept");
    }
    auto other = tagged_tevstr(table, Src::RoomLight, 11);
    if (resolve(table, other.get(), 11) != Src::RoomLight ||
        resolve(table, tevstr.get(), 11) != Src::Unknown)
    {
        append_problem(problems, "a new frame did not clear the old one");
    }
    return problems;
}

std::string test_overflow_is_unknown() {
    auto table = std::make_unique<TagTable>();
    const Vec pos{1.0f, 2.0f, 3.0f};
    std::string problems;
    for (uintptr_t i = 0; i < kTableSlots; ++i) {
        if (!table->note(reinterpret_cast<const void*>(0x10000 + i * 0x388), pos, 5)) {
            append_problem(problems, fmt::format("slot {} refused", i));
            break;
        }
    }
    auto tevstr = tagged_tevstr(*table, Src::SunMoon, 5);
    if (table->overflow() != 1 || resolve(*table, tevstr.get(), 5) != Src::Unknown) {
        append_problem(problems, fmt::format("overflow {} kept the tag", table->overflow()));
    }
    if (!table->unchanged(reinterpret_cast<const void*>(0x10000), pos, 5)) {
        append_problem(problems, "a stored tag was lost");
    }
    return problems;
}

std::string test_flame_writer() {
    LiveTags& tags = live();
    const Src saved = tags.flame;
    std::string problems;
    note_flame_writer(Src::Lantern);
    if (flame_source() != Src::Lantern) {
        append_problem(problems, "lantern not kept");
    }
    note_flame_writer(Src::EffectLight);
    if (flame_source() != Src::EffectLight) {
        append_problem(problems, "effect light not kept");
    }
    note_flame_writer(Src::SunMoon);
    if (flame_source() != Src::Unknown) {
        append_problem(problems, "a non-flame writer is not Unknown");
    }
    tags.flame = saved;
    return problems;
}

}  // namespace

void set_tag(dKy_tevstr_c* tevstr, Src src) noexcept {
    tevstr->dusk_shadowSrc = static_cast<u8>(src);
}

void note_tag(const dKy_tevstr_c* tevstr) noexcept {
    LiveTags& tags = live();
    const u32 frame = aurora::gfx::current_frame();
    roll(tags, frame);
    TagCounts& counts = tags.current;
    if (!tags.table.note(tevstr, tevstr->mLightPosWorld, frame)) {
        ++counts.overflow;
    }
    const Src src = checked(tevstr->dusk_shadowSrc);
    ++counts.tagged;
    ++counts.sources[static_cast<size_t>(src)];
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player != nullptr && tevstr == &player->tevStr) {
        counts.link = src;
        counts.linkTagged = true;
    }
}

void note_base_light(Src src) noexcept {
    live().baseLight = src;
}

Src base_light_source() noexcept {
    return live().baseLight;
}

void note_flame_writer(Src src) noexcept {
    live().flame = src == Src::Lantern || src == Src::EffectLight ? src : Src::Unknown;
}

Src flame_source() noexcept {
    return live().flame;
}

Src entry_source(const dKy_tevstr_c* tevstr) noexcept {
    return resolve(live().table, tevstr, aurora::gfx::current_frame());
}

std::string_view source_name(Src src) noexcept {
    return kSourceNames[static_cast<size_t>(checked(static_cast<u8>(src)))];
}

bool command(std::span<const std::string> args, std::vector<std::string>& out) {
    LiveTags& tags = live();
    u32 count = 0;
    if (args.empty()) {
        const TagCounts& last = tags.last;
        out.push_back(fmt::format("render.fakeshadows: frame {} tagged {} overflow {} link {}{}",
            last.frame, last.tagged, last.overflow,
            last.linkTagged ? source_name(last.link) : "none", counts_text(last)));
        out.push_back(fmt::format("render.fakeshadows: base light {} flame {} table overflow {}",
            source_name(tags.baseLight), source_name(tags.flame), tags.table.overflow()));
    } else if (args.size() == 2 && args[0] == "log" && parse_count(args[1], count)) {
        tags.logUntil = aurora::gfx::current_frame() + count;
        out.push_back(fmt::format("render.fakeshadows: logging until frame {}", tags.logUntil));
    } else {
        out.emplace_back(kUsage);
        return false;
    }
    return true;
}

void self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("fake_shadows.tevstr_layout", test_tevstr_layout());
    check("fake_shadows.source_mirror", test_source_mirror());
    check("fake_shadows.override_is_unknown", test_override_is_unknown());
    check("fake_shadows.copied_tevstr_is_unknown", test_copied_tevstr_is_unknown());
    check("fake_shadows.stale_stamp_is_unknown", test_stale_stamp_is_unknown());
    check("fake_shadows.overflow_is_unknown", test_overflow_is_unknown());
    check("fake_shadows.flame_writer", test_flame_writer());
}

}  // namespace dusk::render::fake_shadows
