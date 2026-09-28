#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RFC 4648 base64 (abseil is not exported to mods).
namespace twili::base64 {

std::string encode(std::span<const uint8_t> data);
// Padding optional.
bool decode(std::string_view text, std::vector<uint8_t>& out);

}  // namespace twili::base64
