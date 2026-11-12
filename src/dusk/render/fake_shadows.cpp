#include "dusk/render/fake_shadows.hpp"

#include "dusk/render/draw_tags.hpp"
#include "dusk/render/model_registry.hpp"
#include "dusk/render/render_bridge.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_drawlist.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_tev_str.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"

#include <absl/container/flat_hash_map.h>
#include <aurora/gfx.hpp>
#include <aurora/render.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <utility>

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
constexpr std::string_view kUsage = "Usage: render.fakeshadows [log <n>|stats|reset|hash]";
constexpr size_t kActorModelCap = 16;
constexpr u64 kKeyReal = 1ull << 56;
constexpr u64 kKeyBlob = 2ull << 56;
constexpr u64 kKeyBridge = 3ull << 56;
constexpr u64 kKeySlot = 1ull << 40;
constexpr u64 kFnvOffset = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;
constexpr u32 kClassShift = 3;
constexpr u32 kDecisionShift = 6;

using aurora::render::GameShadowCaster;
using aurora::render::GameShadowEntry;
using aurora::render::ShadowClass;
using aurora::render::ShadowDecision;
using aurora::render::ShadowDecisionCount;
static_assert(static_cast<u8>(Class::Real) == static_cast<u8>(ShadowClass::Real));
static_assert(static_cast<u8>(Class::Blob) == static_cast<u8>(ShadowClass::Blob));
static_assert(static_cast<u8>(Class::Cue) == static_cast<u8>(ShadowClass::Cue));
static_assert(static_cast<u8>(Class::Telegraph) == static_cast<u8>(ShadowClass::Telegraph));
static_assert(static_cast<u8>(Class::GroundMark) == static_cast<u8>(ShadowClass::GroundMark));
static_assert(static_cast<u8>(Class::Bridge) == static_cast<u8>(ShadowClass::Bridge));

constexpr std::array<std::string_view, ClassCount> kClassNames{
    "Real", "Blob", "Cue", "Telegraph", "GroundMark", "Bridge"};
constexpr std::array<std::string_view, ShadowDecisionCount> kDecisionNames{"None", "Skip",
    "KeepSetting", "KeepNoConsumer", "KeepCoverage", "KeepSource", "KeepCaster",
    "KeepCasterUnknown", "KeepClass", "KeepInterior", "KeepHold", "KeepOverflow"};

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

u32 fake_shadows_log_until() noexcept {
    return live().logUntil;
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

struct ActorRecord {
    u32 id = 0;
    s16 profname = -1;
    u8 blobs = 0;
    u8 models = 0;
    bool truncated = false;
    std::array<const J3DModel*, kActorModelCap> list{};

    void add(const J3DModel* model) noexcept {
        const auto end = list.begin() + models;
        if (model == nullptr || std::find(list.begin(), end, model) != end) {
            return;
        }
        if (models == kActorModelCap) {
            truncated = true;
            return;
        }
        list[models++] = model;
    }

    std::span<const J3DModel* const> entered() const noexcept { return {list.data(), models}; }
};

class ActorTable {
public:
    void clear() noexcept {
        m_records.clear();
        m_index.clear();
    }

    ActorRecord& open(u32 id, s16 profname) {
        const auto [it, inserted] = m_index.try_emplace(id, static_cast<u32>(m_records.size()));
        if (inserted) {
            m_records.push_back({.id = id, .profname = profname});
        }
        return m_records[it->second];
    }

    ActorRecord* find(u32 id) noexcept {
        const auto it = m_index.find(id);
        return it == m_index.end() ? nullptr : &m_records[it->second];
    }

private:
    std::vector<ActorRecord> m_records;
    absl::flat_hash_map<u32, u32> m_index;
};

struct DrawState {
    u32 pass = 1;
    ActorTable actors;
    std::vector<u32> stack;
    Class blobClass = Class::Blob;
};

void push_actor(DrawState& state, u32 id, s16 profname) {
    state.actors.open(id, profname);
    state.stack.push_back(id);
}

void pop_actor(DrawState& state) noexcept {
    if (!state.stack.empty()) {
        state.stack.pop_back();
    }
}

void add_actor_model(DrawState& state, const J3DModel* model) noexcept {
    if (state.stack.empty()) {
        return;
    }
    if (ActorRecord* record = state.actors.find(state.stack.back())) {
        record->add(model);
    }
}

u32 open_actor(const DrawState& state) noexcept {
    return state.stack.empty() ? 0 : state.stack.back();
}

DrawState& draw_live() noexcept {
    static DrawState& state = *new DrawState;
    return state;
}

struct DecideCounts {
    u64 frames = 0;
    u64 entries = 0;
    u64 truncated = 0;
    std::array<u64, ClassCount> entriesByClass{};
    std::array<u64, ClassCount> skippedByClass{};
    std::array<std::array<u64, ShadowDecisionCount>, SrcCount> bySource{};
    std::array<std::array<u64, ShadowDecisionCount>, ClassCount> byClass{};
};

struct DecideLive {
    std::vector<GameShadowCaster> casters;
    DecideCounts counts;
    aurora::render::GameShadowCounts gateBase;
    u64 hash = kFnvOffset;
    u64 hashFrames = 0;
};

DecideLive& decide_live() noexcept {
    static DecideLive& state = *new DecideLive;
    return state;
}

u64 hash_bytes(u64 hash, u64 value, int bytes) noexcept {
    for (int i = 0; i < bytes; ++i) {
        hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * kFnvPrime;
    }
    return hash;
}

Class checked_class(u8 value) noexcept {
    return value < ClassCount ? static_cast<Class>(value) : Class::Blob;
}

ShadowDecision checked_decision(u8 value) noexcept {
    return value < ShadowDecisionCount ? static_cast<ShadowDecision>(value) : ShadowDecision::None;
}

u32 sub_id(Src src, Class cls, ShadowDecision decision) noexcept {
    return static_cast<u32>(src) | static_cast<u32>(cls) << kClassShift |
           static_cast<u32>(decision) << kDecisionShift;
}

u64 real_key(u32 serial, int slot) noexcept {
    return kKeyReal | (serial != 0 ? serial : kKeySlot | static_cast<u32>(slot));
}

u64 blob_key(u32 actor, u8 ordinal) noexcept {
    return kKeyBlob | static_cast<u64>(actor) << 8 | ordinal;
}

template <typename SerialFn, typename EnteredFn>
void fill_casters(std::span<const J3DModel* const> models, std::vector<GameShadowCaster>& out,
    SerialFn&& serial, EnteredFn&& entered) {
    out.clear();
    for (const J3DModel* model : models) {
        out.push_back({make_object_id(ObjectKind::J3dModel, serial(model)), entered(model)});
    }
}

void fill_live_casters(
    std::span<const J3DModel* const> models, std::vector<GameShadowCaster>& out) {
    fill_casters(models, out, model_serial, [](const J3DModel* model) {
        return model_entered_this_frame(model) && registry_castable(model);
    });
}

void apply_real(dDlst_shadowReal_c& real, ShadowDecision decision) noexcept {
    real.mDuskDecision = static_cast<u8>(decision);
    if (decision == ShadowDecision::Skip) {
        real.mState = 2;
    }
}

ShadowDecision decide(DecideLive& live, const GameShadowEntry& entry) noexcept {
    const ShadowDecision decision = aurora::render::decide_game_shadow(entry);
    DecideCounts& counts = live.counts;
    const auto cls = static_cast<size_t>(entry.cls);
    const auto src = static_cast<size_t>(checked(static_cast<u8>(entry.source)));
    const auto d = static_cast<size_t>(decision);
    ++counts.entries;
    ++counts.entriesByClass[cls];
    counts.skippedByClass[cls] += decision == ShadowDecision::Skip ? 1 : 0;
    counts.truncated += entry.castersTruncated ? 1 : 0;
    ++counts.bySource[src][d];
    ++counts.byClass[cls][d];
    live.hash = hash_bytes(live.hash, entry.key, 8);
    live.hash = hash_bytes(live.hash, d, 1);
    return decision;
}

std::string actor_name(DrawState& draw, u32 id, u32 player) {
    if (id == 0) {
        return "none";
    }
    if (id == player) {
        return "Link";
    }
    const ActorRecord* record = draw.actors.find(id);
    const char* name = record != nullptr ? dStage_getName(record->profname, -1) : nullptr;
    return name != nullptr ? std::string{name} : fmt::format("proc{}", id);
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

std::string test_skip_keeps_packing() {
    auto control = std::make_unique<dDlst_shadowControl_c>();
    for (int i = 0; i < 5; ++i) {
        dDlst_shadowReal_c& real = control->mReal[i];
        real.mState = 1;
        real.setZsortPre(i > 0 ? &control->mReal[i - 1] : nullptr);
        real.setZsortNext(i < 4 ? &control->mReal[i + 1] : nullptr);
    }
    control->field_0x4 = &control->mReal[0];
    apply_real(control->mReal[2], ShadowDecision::Skip);
    apply_real(control->mReal[3], ShadowDecision::KeepHold);
    std::string problems;
    std::vector<int> drawn;
    int chan = 0;
    for (dDlst_shadowReal_c* real = control->field_0x4; real != nullptr;
        real = real->getZsortNext())
    {
        if (real->isUse()) {
            drawn.push_back(static_cast<int>(real - control->mReal) * 4 + chan);
            chan = (chan + 1) % 4;
        }
    }
    if (drawn != std::vector<int>{0, 5, 14, 19}) {
        append_problem(problems, fmt::format("{} entries drawn or channels shifted", drawn.size()));
    }
    dDlst_shadowReal_c& skipped = control->mReal[2];
    if (skipped.isUse() || skipped.isNoUse() || skipped.mDuskDecision != 1 ||
        control->mReal[3].mState != 1)
    {
        append_problem(problems, "a skip is not out of both loops or a keep changed state");
    }
    control->mReal[2].reset();
    if (!control->mReal[2].isNoUse()) {
        append_problem(problems, "reset does not free a skipped entry");
    }
    return problems;
}

std::string test_blob_scope_resets() {
    DrawState& state = draw_live();
    const Class saved = state.blobClass;
    std::string problems;
    {
        const BlobClassScope outer(Class::Cue);
        if (state.blobClass != Class::Cue) {
            append_problem(problems, "the scope did not set its class");
        }
        {
            const BlobClassScope inner(Class::Telegraph);
            if (state.blobClass != Class::Telegraph) {
                append_problem(problems, "a nested scope did not set its class");
            }
        }
        if (state.blobClass != Class::Cue) {
            append_problem(problems, "a nested scope did not restore the outer class");
        }
    }
    if (state.blobClass != saved) {
        append_problem(problems, "the class outlived its scope");
    }
    const u32 id = sub_id(Src::SunMoon, Class::Cue, ShadowDecision::KeepHold);
    if ((id & 7) != 2 || ((id >> kClassShift) & 7) != 2 || ((id >> kDecisionShift) & 15) != 10) {
        append_problem(problems, fmt::format("subId 0x{:x} packs wrong", id));
    }
    return problems;
}

std::string test_casters_every_model() {
    const auto* a = reinterpret_cast<const J3DModel*>(uintptr_t{0x1000});
    const auto* b = reinterpret_cast<const J3DModel*>(uintptr_t{0x2000});
    const auto* c = reinterpret_cast<const J3DModel*>(uintptr_t{0x3000});
    const std::array<const J3DModel*, 3> models{a, b, c};
    std::vector<GameShadowCaster> casters;
    fill_casters(
        models, casters,
        [](const J3DModel* m) { return static_cast<u32>(reinterpret_cast<uintptr_t>(m) >> 12); },
        [b](const J3DModel* m) { return m == b; });
    if (casters.size() != 3) {
        return fmt::format("{} casters for 3 models", casters.size());
    }
    std::string problems;
    for (size_t i = 0; i < 3; ++i) {
        if (casters[i].id != make_object_id(ObjectKind::J3dModel, static_cast<u32>(i + 1)) ||
            casters[i].enteredNow != (i == 1))
        {
            append_problem(problems, fmt::format("caster {} wrong", i));
        }
    }
    if (real_key(0, 3) == real_key(3, 0) || real_key(7, 1) != real_key(7, 2) ||
        blob_key(5, 1) == blob_key(5, 2) || (blob_key(5, 1) >> 56) != 2)
    {
        append_problem(problems, "entry keys collide or depend on the slot");
    }
    return problems;
}

std::string test_blob_cap_truncates() {
    ActorRecord record;
    for (uintptr_t i = 1; i <= kActorModelCap + 1; ++i) {
        record.add(reinterpret_cast<const J3DModel*>(i * 0x100));
        record.add(reinterpret_cast<const J3DModel*>(i * 0x100));
    }
    std::string problems;
    if (record.models != kActorModelCap || !record.truncated) {
        append_problem(
            problems, fmt::format("{} models, truncated {}", record.models, record.truncated));
    }
    ActorRecord small;
    small.add(reinterpret_cast<const J3DModel*>(uintptr_t{0x100}));
    small.add(nullptr);
    if (small.models != 1 || small.truncated) {
        append_problem(problems, "a null model counted");
    }
    return problems;
}

std::string test_actor_scope_nesting() {
    DrawState state;
    const auto* m1 = reinterpret_cast<const J3DModel*>(uintptr_t{0x100});
    const auto* m2 = reinterpret_cast<const J3DModel*>(uintptr_t{0x200});
    const auto* m3 = reinterpret_cast<const J3DModel*>(uintptr_t{0x300});
    push_actor(state, 11, 1);
    add_actor_model(state, m1);
    push_actor(state, 22, 2);
    add_actor_model(state, m2);
    pop_actor(state);
    add_actor_model(state, m3);
    pop_actor(state);
    add_actor_model(state, m2);
    pop_actor(state);
    std::string problems;
    const ActorRecord* outer = state.actors.find(11);
    const ActorRecord* inner = state.actors.find(22);
    if (outer == nullptr || outer->models != 2 || outer->list[0] != m1 || outer->list[1] != m3) {
        append_problem(problems, "the outer actor lost its models or took the inner one's");
    }
    if (inner == nullptr || inner->models != 1 || inner->list[0] != m2) {
        append_problem(problems, "the inner actor's models are wrong");
    }
    if (open_actor(state) != 0) {
        append_problem(problems, "an actor stayed open");
    }
    push_actor(state, 11, 1);
    if (state.actors.find(11)->models != 2) {
        append_problem(problems, "reopening an actor in the same pass dropped its models");
    }
    return problems;
}

aurora::render::GameShadowCounts since(
    aurora::render::GameShadowCounts now, const aurora::render::GameShadowCounts& base) noexcept {
    for (size_t d = 0; d < ShadowDecisionCount; ++d) {
        now.decisions[d] -= base.decisions[d];
        now.safetyReturns[d] -= base.safetyReturns[d];
    }
    now.entries -= base.entries;
    now.missingCasterLatch -= base.missingCasterLatch;
    now.missingCasterCast -= base.missingCasterCast;
    now.doubleDarkEntries -= base.doubleDarkEntries;
    now.userKept -= base.userKept;
    now.transitions -= base.transitions;
    now.flipViolations -= base.flipViolations;
    now.provisional -= base.provisional;
    now.provisionalFailed -= base.provisionalFailed;
    now.partialCaster -= base.partialCaster;
    now.overflow -= base.overflow;
    now.sunLossFrames -= base.sunLossFrames;
    return now;
}

std::string decision_counts(const std::array<u64, ShadowDecisionCount>& counts) {
    std::string text;
    for (size_t d = 0; d < ShadowDecisionCount; ++d) {
        if (counts[d] != 0) {
            text += fmt::format(" {} {}", kDecisionNames[d], counts[d]);
        }
    }
    return text.empty() ? " none" : text;
}

void describe_stats(std::vector<std::string>& out) {
    const DecideCounts& c = decide_live().counts;
    out.push_back(fmt::format("render.fakeshadows stats: frames {} entries {} truncated {}",
        c.frames, c.entries, c.truncated));
    for (size_t i = 0; i < ClassCount; ++i) {
        if (c.entriesByClass[i] != 0) {
            out.push_back(fmt::format("render.fakeshadows stats: class {} entries {} skipped {}:{}",
                kClassNames[i], c.entriesByClass[i], c.skippedByClass[i],
                decision_counts(c.byClass[i])));
        }
    }
    for (size_t i = 0; i < SrcCount; ++i) {
        const u64 total = std::accumulate(c.bySource[i].begin(), c.bySource[i].end(), u64{0});
        if (total != 0) {
            out.push_back(fmt::format("render.fakeshadows stats: source {} entries {}:{}",
                kSourceNames[i], total, decision_counts(c.bySource[i])));
        }
    }
    const aurora::render::GameShadowStats stats = aurora::render::game_shadow_stats(false);
    const aurora::render::GameShadowCounts t = since(stats.total, decide_live().gateBase);
    out.push_back(fmt::format("render.fakeshadows gate: frame {} entries {}:{}", stats.frame,
        t.entries, decision_counts(t.decisions)));
    out.push_back(fmt::format("render.fakeshadows gate: missingCaster latch {} cast {} "
                              "castBitsPending {} doubleDarkEntries {} userKept {} "
                              "transitions {} flipViolations {} provisional {} "
                              "provisionalFailed {} partialCaster {} overflow {} "
                              "sunLossFrames {} safetyReturnsPerKeyMax {}",
        t.missingCasterLatch, t.missingCasterCast, stats.current.castBitsPending,
        t.doubleDarkEntries, t.userKept, t.transitions, t.flipViolations, t.provisional,
        t.provisionalFailed, t.partialCaster, t.overflow, t.sunLossFrames,
        t.safetyReturnsPerKeyMax));
    out.push_back(
        fmt::format("render.fakeshadows gate: safetyReturns{}", decision_counts(t.safetyReturns)));
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

u32 draw_pass() noexcept {
    return draw_live().pass;
}

void begin_draw_pass() noexcept {
    DrawState& state = draw_live();
    ++state.pass;
    state.actors.clear();
}

void note_actor_model(const J3DModel* model) noexcept {
    add_actor_model(draw_live(), model);
}

void note_real(dDlst_shadowReal_c* real, const dKy_tevstr_c* tevstr) noexcept {
    real->mDuskSrc = static_cast<u8>(entry_source(tevstr));
    real->mDuskActor = open_actor(draw_live());
    real->mDuskDecision = static_cast<u8>(ShadowDecision::None);
}

void note_blob(dDlst_shadowSimple_c* blob) noexcept {
    DrawState& state = draw_live();
    const u32 actor = open_actor(state);
    ActorRecord& record = state.actors.open(actor, -1);
    blob->mDuskActor = actor;
    blob->mDuskClass = static_cast<u8>(state.blobClass);
    blob->mDuskOrdinal = record.blobs;
    blob->mDuskDecision = static_cast<u8>(ShadowDecision::None);
    blob->mDuskSkip = false;
    if (record.blobs < UINT8_MAX) {
        ++record.blobs;
    }
}

void decide_frame(dDlst_shadowControl_c& control) noexcept {
    DecideLive& live = decide_live();
    DrawState& draw = draw_live();
    const u32 frame = aurora::gfx::current_frame();
    const u32 until = fake_shadows_log_until();
    const bool log = until != 0 && frame <= until;
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const u32 playerId = player != nullptr ? fopAcM_GetID(player) : 0;
    std::string reals;
    std::array<u32, ShadowDecisionCount> blobDecisions{};
    u32 entries = 0;
    for (int i = 0; i < 8; ++i) {
        dDlst_shadowReal_c& real = control.mReal[i];
        if (!real.isUse()) {
            continue;
        }
        fill_live_casters({real.mpModels, real.mModelNum}, live.casters);
        const GameShadowEntry entry{.key = real_key(model_serial(real.mpModels[0]), i),
            .source = static_cast<ShadowSource>(checked(real.mDuskSrc)),
            .cls = ShadowClass::Real,
            .casters = live.casters};
        const ShadowDecision decision = decide(live, entry);
        apply_real(real, decision);
        ++entries;
        if (log) {
            const auto entered = std::ranges::count_if(
                live.casters, [](const GameShadowCaster& c) { return c.enteredNow; });
            reals += fmt::format(" {}/{}/{}/{}e{}", actor_name(draw, real.mDuskActor, playerId),
                kSourceNames[static_cast<size_t>(checked(real.mDuskSrc))],
                kDecisionNames[static_cast<size_t>(decision)], live.casters.size(), entered);
        }
    }
    const Src blobSource = base_light_source();
    for (int i = 0; i < control.mSimpleNum; ++i) {
        dDlst_shadowSimple_c& blob = control.mSimple[i];
        const ActorRecord* record =
            blob.mDuskActor != 0 ? draw.actors.find(blob.mDuskActor) : nullptr;
        if (record != nullptr) {
            fill_live_casters(record->entered(), live.casters);
        } else {
            live.casters.clear();
        }
        const GameShadowEntry entry{.key = blob_key(blob.mDuskActor, blob.mDuskOrdinal),
            .source = static_cast<ShadowSource>(blobSource),
            .cls = static_cast<ShadowClass>(checked_class(blob.mDuskClass)),
            .casters = live.casters,
            .castersTruncated = record != nullptr && record->truncated};
        const ShadowDecision decision = decide(live, entry);
        blob.mDuskDecision = static_cast<u8>(decision);
        blob.mDuskSkip = decision == ShadowDecision::Skip;
        ++blobDecisions[static_cast<size_t>(decision)];
        ++entries;
    }
    live.hash = hash_bytes(live.hash, entries, 4);
    ++live.hashFrames;
    ++live.counts.frames;
    if (log) {
        const aurora::render::GateState gate = aurora::render::gate_state();
        std::string blobs;
        for (size_t d = 0; d < ShadowDecisionCount; ++d) {
            if (blobDecisions[d] != 0) {
                blobs += fmt::format(" {} {}", kDecisionNames[d], blobDecisions[d]);
            }
        }
        FakeLog.info("render.fakeshadows decide: frame {} consumer {} prev {} coverage {} sun {} "
                     "interior {} game {} reals{} blobs {}{}",
            frame, gate.consumer, gate.prevResolveRan, gate.coverageComplete, gate.sunGeometric,
            gate.interior, static_cast<int>(gate.game), reals, control.mSimpleNum, blobs);
    }
}

bool bridge_skipped(
    const J3DModel* shadow, const J3DModel* caster, const dKy_tevstr_c* tevstr) noexcept {
    DecideLive& live = decide_live();
    const std::array<const J3DModel*, 1> models{caster};
    fill_live_casters(models, live.casters);
    const GameShadowEntry entry{.key = kKeyBridge | model_serial(shadow),
        .source = static_cast<ShadowSource>(entry_source(tevstr)),
        .cls = ShadowClass::Bridge,
        .casters = live.casters};
    return decide(live, entry) == ShadowDecision::Skip;
}

u32 real_sub_id(const dDlst_shadowReal_c& real) noexcept {
    return sub_id(checked(real.mDuskSrc), Class::Real, checked_decision(real.mDuskDecision));
}

u32 blob_sub_id(const dDlst_shadowSimple_c& blob) noexcept {
    return sub_id(
        base_light_source(), checked_class(blob.mDuskClass), checked_decision(blob.mDuskDecision));
}

ActorDrawScope::ActorDrawScope(const fopAc_ac_c* actor) noexcept {
    push_actor(
        draw_live(), fopAcM_GetID(actor), static_cast<const base_process_class*>(actor)->profname);
}

ActorDrawScope::~ActorDrawScope() {
    pop_actor(draw_live());
}

BlobClassScope::BlobClassScope(Class cls) noexcept
    : mPrevious(std::exchange(draw_live().blobClass, cls)) {}

BlobClassScope::~BlobClassScope() {
    draw_live().blobClass = mPrevious;
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
    } else if (args.size() == 1 && args[0] == "stats") {
        describe_stats(out);
    } else if (args.size() == 1 && args[0] == "reset") {
        DecideLive& state = decide_live();
        state.counts = {};
        state.gateBase = aurora::render::game_shadow_stats(false).total;
        state.hash = kFnvOffset;
        state.hashFrames = 0;
        out.push_back(fmt::format("render.fakeshadows: counters and hash reset at frame {}",
            aurora::gfx::current_frame()));
    } else if (args.size() == 1 && args[0] == "hash") {
        const DecideLive& state = decide_live();
        out.push_back(fmt::format("render.fakeshadows hash: 0x{:016x} over {} frames, {} entries",
            state.hash, state.hashFrames, state.counts.entries));
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
    check("fake_shadows.skip_keeps_packing", test_skip_keeps_packing());
    check("fake_shadows.blob_scope_resets", test_blob_scope_resets());
    check("fake_shadows.casters_every_model", test_casters_every_model());
    check("fake_shadows.blob_cap_truncates", test_blob_cap_truncates());
    check("fake_shadows.actor_scope_nesting", test_actor_scope_nesting());
}

}  // namespace dusk::render::fake_shadows
