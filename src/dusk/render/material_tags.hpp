#pragma once

#include <dolphin/gx/GXAurora.h>
#include <dolphin/types.h>

#include <string>
#include <vector>

class J3DModelData;

namespace dusk::render {

struct SelfTestReport;

struct MaterialTag {
    u32 categories = 0;
    u32 pendingCategories = 0;
    AuroraTevPolicy tevPolicy{};
};

bool matches_ma_code(const char* name, const char* code) noexcept;
bool matches_cloud_scroll(const char* name) noexcept;
bool matches_k1_writer(const char* name) noexcept;
bool is_room_archive(const char* archive) noexcept;
MaterialTag material_tag_for_name(const char* name) noexcept;
bool compute_material_tags(const J3DModelData* data, std::vector<MaterialTag>& out);
std::string category_names(u32 categories);
std::string tev_policy_text(const AuroraTevPolicy& policy);
bool pending_tags_enabled() noexcept;
void set_pending_tags_enabled(bool enabled) noexcept;
void material_tags_self_test(SelfTestReport& report);

}  // namespace dusk::render
