#pragma once

#include <dolphin/types.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

class J3DModel;
class dDlst_shadowControl_c;
class dDlst_shadowReal_c;
class dDlst_shadowSimple_c;
class dKy_tevstr_c;
class fopAc_ac_c;

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

enum class Class : u8 {
    Real,
    Blob,
    Cue,
    Telegraph,
    GroundMark,
    Bridge,
};

inline constexpr size_t ClassCount = static_cast<size_t>(Class::Bridge) + 1;

void set_tag(dKy_tevstr_c* tevstr, Src src) noexcept;
void note_tag(const dKy_tevstr_c* tevstr) noexcept;
void note_base_light(Src src) noexcept;
Src base_light_source() noexcept;
void note_flame_writer(Src src) noexcept;
Src flame_source() noexcept;
Src entry_source(const dKy_tevstr_c* tevstr) noexcept;
std::string_view source_name(Src src) noexcept;

u32 draw_pass() noexcept;
void begin_draw_pass() noexcept;
void note_actor_model(const J3DModel* model) noexcept;
void note_real(dDlst_shadowReal_c* real, const dKy_tevstr_c* tevstr) noexcept;
void note_blob(dDlst_shadowSimple_c* blob) noexcept;
void decide_frame(dDlst_shadowControl_c& control) noexcept;
bool bridge_skipped(
    const J3DModel* shadow, const J3DModel* caster, const dKy_tevstr_c* tevstr) noexcept;
u32 real_sub_id(const dDlst_shadowReal_c& real) noexcept;
u32 blob_sub_id(const dDlst_shadowSimple_c& blob) noexcept;

class ActorDrawScope {
public:
    [[nodiscard]] explicit ActorDrawScope(const fopAc_ac_c* actor) noexcept;
    ~ActorDrawScope();
    ActorDrawScope(const ActorDrawScope&) = delete;
    ActorDrawScope& operator=(const ActorDrawScope&) = delete;
};

class BlobClassScope {
public:
    [[nodiscard]] explicit BlobClassScope(Class cls) noexcept;
    ~BlobClassScope();
    BlobClassScope(const BlobClassScope&) = delete;
    BlobClassScope& operator=(const BlobClassScope&) = delete;

private:
    Class mPrevious;
};

bool command(std::span<const std::string> args, std::vector<std::string>& out);
void self_test(SelfTestReport& report);

}  // namespace dusk::render::fake_shadows
