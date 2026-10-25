#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dusk::texture_replacements {

// Mod replacements are prioritized *over* user replacements (<data folder>/texture_replacements/)
inline constexpr int32_t kUserTextureReplacementPriority = -1'000'000;
// Pack folders of 2D art: generated mips would only delay their first appearance
inline constexpr std::array<std::string_view, 1> kNoGeneratedMipDirs{"UI"};

void reload();
void shutdown();

}
