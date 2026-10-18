#include "dusk/render/material_tags.hpp"

#include "dusk/render/render_bridge.hpp"

#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/JUtility/JUTNameTab.h"

#include <fmt/format.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace dusk::render {
namespace {

struct CodeRow {
    char code[3];
    u32 categories;
    u32 pendingCategories;
};

constexpr std::array<CodeRow, 8> kCodeRows{{
    {"03", AURORA_CAT_WATER | AURORA_CAT_NO_PBR, 0},
    {"09", AURORA_CAT_WATER | AURORA_CAT_NO_PBR, 0},
    {"17", AURORA_CAT_WATER | AURORA_CAT_NO_PBR, 0},
    {"19", AURORA_CAT_WATER | AURORA_CAT_NO_PBR, 0},
    {"02", AURORA_CAT_SCREEN_READ, 0},
    {"10", AURORA_CAT_SCREEN_READ, 0},
    {"11", AURORA_CAT_NO_PBR, 0},
    {"14", 0, AURORA_CAT_NO_AO},
}};
constexpr u32 kCloudScrollCategories = AURORA_CAT_CLOUD_RECEIVER;
constexpr u32 kReservedCategories = AURORA_CAT_OVERLAY | AURORA_CAT_STATIC;
constexpr u32 kReservedPolicyBits = 0xF0000000u;

static_assert([] {
    for (const auto& row : kCodeRows) {
        if (((row.categories | row.pendingCategories) & kReservedCategories) != 0) {
            return false;
        }
    }
    return (kCloudScrollCategories & kReservedCategories) == 0;
}());

constexpr std::array<const char*, 7> kRegisterNames{"C0", "C1", "C2", "K0", "K1", "K2", "K3"};
constexpr std::array<char, 4> kChannelNames{'r', 'g', 'b', 'a'};

constexpr std::array<std::pair<u32, const char*>, 22> kCategoryNames{{
    {AURORA_CAT_STATIC, "STATIC"},
    {AURORA_CAT_DYNAMIC, "DYNAMIC"},
    {AURORA_CAT_SKINNED, "SKINNED"},
    {AURORA_CAT_TERRAIN, "TERRAIN"},
    {AURORA_CAT_FOLIAGE, "FOLIAGE"},
    {AURORA_CAT_WATER, "WATER"},
    {AURORA_CAT_SKY, "SKY"},
    {AURORA_CAT_PARTICLE, "PARTICLE"},
    {AURORA_CAT_DECAL, "DECAL"},
    {AURORA_CAT_UI, "UI"},
    {AURORA_CAT_PLAYER, "PLAYER"},
    {AURORA_CAT_EMISSIVE, "EMISSIVE"},
    {AURORA_CAT_OVERLAY, "OVERLAY"},
    {AURORA_CAT_NO_SHADOW_CAST, "NO_SHADOW_CAST"},
    {AURORA_CAT_NO_SHADOW_RECEIVE, "NO_SHADOW_RECEIVE"},
    {AURORA_CAT_NO_PBR, "NO_PBR"},
    {AURORA_CAT_NO_CAPTURE, "NO_CAPTURE"},
    {AURORA_CAT_NO_RT, "NO_RT"},
    {AURORA_CAT_SCREEN_READ, "SCREEN_READ"},
    {AURORA_CAT_FAKE_SHADOW, "FAKE_SHADOW"},
    {AURORA_CAT_CLOUD_RECEIVER, "CLOUD_RECEIVER"},
    {AURORA_CAT_NO_AO, "NO_AO"},
}};

// K1.r = 255 saturates the clamp(TEXC + K1.r) cloud term to white, which removes the shadow
constexpr AuroraTevPolicy ma00_policy() noexcept {
    AuroraTevPolicy policy{};
    policy.mask = AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_K1, AURORA_TEV_CH_R);
    policy.neutral[AURORA_TEV_REG_K1][AURORA_TEV_CH_R] = 255;
    return policy;
}

constexpr AuroraTevPolicy ma07_policy() noexcept {
    AuroraTevPolicy policy{};
    policy.mask = AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_R) |
                  AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_G) |
                  AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_B);
    return policy;
}

bool s_pendingTags = false;

// The game reads name[3..6] directly, so anything shorter can never match one of its codes
bool has_code_chars(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (int i = 0; i < 7; ++i) {
        if (name[i] == '\0') {
            return false;
        }
    }
    return true;
}

bool has_tag(const MaterialTag& tag) noexcept {
    return tag.categories != 0 || tag.pendingCategories != 0 || tag.tevPolicy.mask != 0;
}

bool same_policy(const AuroraTevPolicy& a, const AuroraTevPolicy& b) noexcept {
    if (a.mask != b.mask) {
        return false;
    }
    for (size_t reg = 0; reg < 7; ++reg) {
        for (size_t channel = 0; channel < 4; ++channel) {
            if (a.neutral[reg][channel] != b.neutral[reg][channel]) {
                return false;
            }
        }
    }
    return true;
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

struct Expectation {
    const char* name;
    u32 categories;
    u32 pendingCategories;
    u32 mask;
};

void expect_tags(std::string& problems, std::initializer_list<Expectation> expectations) {
    for (const auto& expected : expectations) {
        const MaterialTag tag = material_tag_for_name(expected.name);
        if (tag.categories != expected.categories ||
            tag.pendingCategories != expected.pendingCategories ||
            tag.tevPolicy.mask != expected.mask)
        {
            append_problem(
                problems, fmt::format("'{}' gives {} pending {} policy {}",
                              expected.name == nullptr ? "(null)" : expected.name,
                              category_names(tag.categories), category_names(tag.pendingCategories),
                              tev_policy_text(tag.tevPolicy)));
        }
    }
}

std::string test_water() {
    std::string problems;
    constexpr u32 water = AURORA_CAT_WATER | AURORA_CAT_NO_PBR;
    expect_tags(problems, {{"abcMA03x", water, 0, 0}, {"abcMA09", water, 0, 0},
                              {"abcMA17", water, 0, 0}, {"abcMA19", water, 0, 0}});
    return problems;
}

std::string test_screen_read() {
    std::string problems;
    expect_tags(problems,
        {{"abcMA02", AURORA_CAT_SCREEN_READ, 0, 0}, {"abcMA10", AURORA_CAT_SCREEN_READ, 0, 0}});
    return problems;
}

std::string test_no_pbr() {
    std::string problems;
    expect_tags(problems, {{"abcMA11", AURORA_CAT_NO_PBR, 0, 0}});
    return problems;
}

std::string test_cloud_scroll() {
    std::string problems;
    expect_tags(problems, {{"abcMA00", AURORA_CAT_CLOUD_RECEIVER, 0, ma00_policy().mask},
                              {"abcMA01", AURORA_CAT_CLOUD_RECEIVER, 0, 0},
                              {"abcMA16", AURORA_CAT_CLOUD_RECEIVER, 0, 0},
                              {"abcXY16", AURORA_CAT_CLOUD_RECEIVER, 0, 0}});
    if (!matches_cloud_scroll("abcMA16") || !matches_k1_writer("abcMA16")) {
        append_problem(problems, "MA16 does not match both predicates");
    }
    if (!matches_cloud_scroll("abcXY16") || matches_k1_writer("abcXY16")) {
        append_problem(problems, "xx16 is not a scroll-only match");
    }
    return problems;
}

std::string test_ma04_excluded() {
    std::string problems;
    expect_tags(problems, {{"abcMA04", 0, 0, 0}});
    if (!matches_k1_writer("abcMA04") || matches_cloud_scroll("abcMA04")) {
        append_problem(problems, "MA04 is not a K1 writer outside the scroll set");
    }
    return problems;
}

std::string test_policy_data() {
    std::string problems;
    const MaterialTag ma00 = material_tag_for_name("abcMA00");
    AuroraTevPolicy expected00{};
    expected00.mask = AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_K1, AURORA_TEV_CH_R);
    expected00.neutral[AURORA_TEV_REG_K1][AURORA_TEV_CH_R] = 255;
    if (!same_policy(ma00.tevPolicy, expected00) || ma00.categories != AURORA_CAT_CLOUD_RECEIVER) {
        append_problem(
            problems, fmt::format("MA00 gives {} policy {}", category_names(ma00.categories),
                          tev_policy_text(ma00.tevPolicy)));
    }
    const MaterialTag ma07 = material_tag_for_name("abcMA07");
    AuroraTevPolicy expected07{};
    expected07.mask = AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_R) |
                      AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_G) |
                      AURORA_TEV_POLICY_BIT(AURORA_TEV_REG_C0, AURORA_TEV_CH_B);
    if (!same_policy(ma07.tevPolicy, expected07) || ma07.categories != 0) {
        append_problem(
            problems, fmt::format("MA07 gives {} policy {}", category_names(ma07.categories),
                          tev_policy_text(ma07.tevPolicy)));
    }
    if (tev_policy_text(ma00.tevPolicy) != "K1.r=255" ||
        tev_policy_text(ma07.tevPolicy) != "C0.rgb=0")
    {
        append_problem(problems, "policy text differs");
    }
    for (int code = 0; code < 100; ++code) {
        const std::string name = fmt::format("abcMA{:02}", code);
        if ((material_tag_for_name(name.c_str()).tevPolicy.mask & kReservedPolicyBits) != 0) {
            append_problem(problems, fmt::format("{} sets reserved policy bits", name));
        }
    }
    return problems;
}

std::string test_ma14_pending() {
    std::string problems;
    expect_tags(problems, {{"abcMA14", 0, AURORA_CAT_NO_AO, 0}});
    return problems;
}

std::string test_short_and_null() {
    std::string problems;
    expect_tags(problems, {{nullptr, 0, 0, 0}, {"", 0, 0, 0}, {"abc", 0, 0, 0}, {"abcMA1", 0, 0, 0},
                              {"MA03xyz", 0, 0, 0}});
    for (const char* name : {static_cast<const char*>(nullptr), "", "abc", "abcMA1", "abc16"}) {
        if (matches_cloud_scroll(name) || matches_k1_writer(name) || matches_ma_code(name, "03")) {
            append_problem(problems,
                fmt::format("'{}' matches a predicate", name == nullptr ? "(null)" : name));
        }
    }
    return problems;
}

std::string test_no_overlay() {
    std::string problems;
    for (const auto& row : kCodeRows) {
        if (((row.categories | row.pendingCategories) & kReservedCategories) != 0) {
            append_problem(problems, fmt::format("row MA{} sets OVERLAY or STATIC", row.code));
        }
    }
    std::vector<std::string> names{"abcXY16"};
    for (int code = 0; code < 100; ++code) {
        names.push_back(fmt::format("abcMA{:02}", code));
    }
    for (const auto& name : names) {
        const MaterialTag tag = material_tag_for_name(name.c_str());
        if (((tag.categories | tag.pendingCategories) & kReservedCategories) != 0) {
            append_problem(problems, fmt::format("{} gives OVERLAY or STATIC", name));
        }
    }
    return problems;
}

std::string test_room_archives() {
    std::string problems;
    for (const char* archive : {"R00_00", "R05_00", "R63_00"}) {
        if (!is_room_archive(archive)) {
            append_problem(problems, fmt::format("{} is not a room archive", archive));
        }
    }
    for (const char* archive : {static_cast<const char*>(nullptr), "", "STG_00", "R00_01", "R0_00",
             "R00_00x", "Alink", "RXX_00"})
    {
        if (is_room_archive(archive)) {
            append_problem(problems,
                fmt::format("{} is a room archive", archive == nullptr ? "(null)" : archive));
        }
    }
    return problems;
}

}  // namespace

bool matches_ma_code(const char* name, const char* code) noexcept {
    return has_code_chars(name) && name[3] == 'M' && name[4] == 'A' && name[5] == code[0] &&
           name[6] == code[1];
}

bool matches_cloud_scroll(const char* name) noexcept {
    if (!has_code_chars(name)) {
        return false;
    }
    return (name[3] == 'M' && name[4] == 'A' && name[5] == '0' &&
               (name[6] == '0' || name[6] == '1')) ||
           (name[5] == '1' && name[6] == '6');
}

bool matches_k1_writer(const char* name) noexcept {
    return matches_ma_code(name, "00") || matches_ma_code(name, "01") ||
           matches_ma_code(name, "04") || matches_ma_code(name, "16");
}

// dComIfG_getRoomArcName names every room archive R<nn>_00
bool is_room_archive(const char* archive) noexcept {
    if (archive == nullptr) {
        return false;
    }
    const std::string_view name{archive};
    return name.size() == 6 && name[0] == 'R' && name[1] >= '0' && name[1] <= '9' &&
           name[2] >= '0' && name[2] <= '9' && name.substr(3) == "_00";
}

MaterialTag material_tag_for_name(const char* name) noexcept {
    MaterialTag tag;
    if (!has_code_chars(name)) {
        return tag;
    }
    for (const auto& row : kCodeRows) {
        if (matches_ma_code(name, row.code)) {
            tag.categories |= row.categories;
            tag.pendingCategories |= row.pendingCategories;
        }
    }
    if (matches_cloud_scroll(name)) {
        tag.categories |= kCloudScrollCategories;
    }
    if (matches_ma_code(name, "00")) {
        tag.tevPolicy = ma00_policy();
    } else if (matches_ma_code(name, "07")) {
        tag.tevPolicy = ma07_policy();
    }
    return tag;
}

bool compute_material_tags(const J3DModelData* data, std::vector<MaterialTag>& out) {
    out.clear();
    const JUTNameTab* names = data != nullptr ? data->getMaterialName() : nullptr;
    if (names == nullptr) {
        return false;
    }
    const u16 count = data->getMaterialNum();
    out.resize(count);
    bool tagged = false;
    for (u16 i = 0; i < count; ++i) {
        out[i] = material_tag_for_name(names->getName(i));
        tagged = tagged || has_tag(out[i]);
    }
    if (!tagged) {
        out.clear();
    }
    return tagged;
}

std::string category_names(u32 categories) {
    if (categories == 0) {
        return "none";
    }
    std::string text;
    u32 named = 0;
    for (const auto& [bit, name] : kCategoryNames) {
        if ((categories & bit) != 0) {
            if (!text.empty()) {
                text += '|';
            }
            text += name;
            named |= bit;
        }
    }
    if ((categories & ~named) != 0) {
        if (!text.empty()) {
            text += '|';
        }
        text += fmt::format("0x{:X}", categories & ~named);
    }
    return text;
}

std::string tev_policy_text(const AuroraTevPolicy& policy) {
    std::string text;
    for (u32 reg = 0; reg < kRegisterNames.size(); ++reg) {
        std::array<bool, 4> done{};
        for (u32 channel = 0; channel < 4; ++channel) {
            if (done[channel] || (policy.mask & AURORA_TEV_POLICY_BIT(reg, channel)) == 0) {
                continue;
            }
            const u8 value = policy.neutral[reg][channel];
            std::string channels;
            for (u32 other = channel; other < 4; ++other) {
                if (!done[other] && (policy.mask & AURORA_TEV_POLICY_BIT(reg, other)) != 0 &&
                    policy.neutral[reg][other] == value)
                {
                    channels += kChannelNames[other];
                    done[other] = true;
                }
            }
            if (!text.empty()) {
                text += ' ';
            }
            text += fmt::format("{}.{}={}", kRegisterNames[reg], channels, value);
        }
    }
    if ((policy.mask & kReservedPolicyBits) != 0) {
        if (!text.empty()) {
            text += ' ';
        }
        text += fmt::format("reserved=0x{:08X}", policy.mask & kReservedPolicyBits);
    }
    return text.empty() ? std::string{"none"} : text;
}

bool pending_tags_enabled() noexcept {
    return s_pendingTags;
}

void set_pending_tags_enabled(bool enabled) noexcept {
    s_pendingTags = enabled;
}

void material_tags_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("mattags.water", test_water());
    check("mattags.screen_read", test_screen_read());
    check("mattags.no_pbr", test_no_pbr());
    check("mattags.cloud_scroll", test_cloud_scroll());
    check("mattags.ma04_excluded", test_ma04_excluded());
    check("mattags.policy_data", test_policy_data());
    check("mattags.ma14_pending", test_ma14_pending());
    check("mattags.short_and_null", test_short_and_null());
    check("mattags.no_overlay", test_no_overlay());
    check("mattags.room_archives", test_room_archives());
}

}  // namespace dusk::render
