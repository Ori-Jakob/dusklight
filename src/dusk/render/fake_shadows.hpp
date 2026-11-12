#pragma once

#include <dolphin/types.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

class dKy_tevstr_c;

namespace dusk::render {
struct SelfTestReport;
}

namespace dusk::render::fake_shadows {

enum class Src : u8 {
    Unknown,
    Vertical,
    SunMoon,
    RoomKey,
    PointLight,
    RoomLight,
    Lantern,
    EffectLight,
};

inline constexpr size_t SrcCount = static_cast<size_t>(Src::EffectLight) + 1;

void set_tag(dKy_tevstr_c* tevstr, Src src) noexcept;
void note_tag(const dKy_tevstr_c* tevstr) noexcept;
void note_base_light(Src src) noexcept;
Src base_light_source() noexcept;
void note_flame_writer(Src src) noexcept;
Src flame_source() noexcept;
Src entry_source(const dKy_tevstr_c* tevstr) noexcept;
std::string_view source_name(Src src) noexcept;

bool command(std::span<const std::string> args, std::vector<std::string>& out);
void self_test(SelfTestReport& report);

}  // namespace dusk::render::fake_shadows
