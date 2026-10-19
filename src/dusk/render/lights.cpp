#include "dusk/render/lights.hpp"

#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/render/fog_probe.hpp"
#include "dusk/render/hints.hpp"
#include "dusk/render/render_bridge.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_stage.h"
#include "f_op/f_op_camera_mng.h"

#include <aurora/gfx.hpp>
#include <aurora/render.hpp>
#include <borealis/log.hpp>
#include <dolphin/gx/GXAurora.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>

// Defined in d_kankyo.cpp without a header declaration
int dKy_light_influence_id(cXyz position, int param_1);
BOOL dKy_lightswitch_check(stage_pure_lightvec_info_class* stage_light_info_p, char room_no);

namespace dusk::render {
namespace {

namespace ar = aurora::render;

constexpr borealis::Log LightLog{"dusk::render"};

enum class LightKind : u8 {
    None,
    Sun,
    Moon,
    KeyRoom,
    CameraFill,
    PlayerLight,
    Reserved,
    BossSpot,
    TwilightCamera,
    WaterCamera,
    Point,
    Effect,
    ThunderFlash,
    Room,
};
constexpr size_t kKindCount = 14;
static_assert(static_cast<size_t>(LightKind::Room) + 1 == kKindCount);

enum class SlotState : u8 {
    Empty,
    Emitted,
    Disabled,
    Duplicate,
    Queued,
    Replaced,
    Key,
    Unused,
    Dropped,
};
constexpr size_t kSlotStateCount = 9;
static_assert(static_cast<size_t>(SlotState::Dropped) + 1 == kSlotStateCount);

enum class SlotTableId : u8 {
    Point,
    Effect,
    Dungeon,
    EnvSlot,
    Queued,
};

constexpr int kPointCount = 100;
constexpr int kEffectCount = 5;
constexpr int kDungeonCount = 8;
constexpr int kEnvSlotCount = 8;
constexpr int kEnvSlotsUsed = 6;
constexpr int kQueuedCount = 6;
constexpr int kRoomCount = 64;
constexpr int kRoomLights = 6;
constexpr size_t kReportSlots =
    kPointCount + kEffectCount + kDungeonCount + kEnvSlotCount + kQueuedCount;
constexpr size_t kIdSlots = kRoomCount * kRoomLights;

static_assert(sizeof(dScnKy_env_light_c::pointlight) / sizeof(LIGHT_INFLUENCE*) == kPointCount);
static_assert(sizeof(dScnKy_env_light_c::efplight) / sizeof(LIGHT_INFLUENCE*) == kEffectCount);
static_assert(sizeof(dScnKy_env_light_c::dungeonlight) / sizeof(DUNGEON_LIGHT) == kDungeonCount);
static_assert(sizeof(dScnKy_env_light_c::field_0x0c18) / sizeof(BOSS_LIGHT) == kEnvSlotCount);
static_assert(sizeof(dScnKy_env_light_c::field_0x0d58) / sizeof(BOSS_LIGHT) == kQueuedCount);
static_assert(kReportSlots == 127);

constexpr double kPi = 3.14159265358979323846;
constexpr f32 kRefBrightness = 0.99999f;
// GxFogSet_Sub's planes when there is no view or its planes are unusable
constexpr f32 kNoViewNear = 1.0f;
constexpr f32 kNoViewFar = 160000.0f;
// The game's far plane when it has no view, so an unattenuated light reaches all it can see
constexpr f32 kMaxLightRadius = kNoViewFar;
// The GX radius of a point light is about 5x the distance actors are lit to; M10 tunes this
constexpr f32 kPointRadiusScale = 1.0f;
constexpr u32 kCameraFillMask = AURORA_CAT_DYNAMIC | AURORA_CAT_PLAYER;
constexpr f32 kSunRise = 67.5f;
constexpr f32 kSunSet = 292.5f;
constexpr f32 kCrossFade = 15.0f;
constexpr f32 kSkyDistance = 80000.0f;
constexpr f32 kSkyDepth = -48000.0f;
constexpr f32 kEnvSlotLift = 200.0f;
// Every hint is the FIFO header and a u16 version, then its payload
constexpr u32 kHintPrefixBytes = GX_AURORA_HINT_HEADER_SIZE + sizeof(u16);
constexpr u32 kLightsFixedBytes = kHintPrefixBytes + sizeof(u16);
constexpr u32 kLightBytes = sizeof(AuroraLight);
constexpr u32 kEnvironmentHintBytes = kHintPrefixBytes + sizeof(AuroraEnvironment);
static_assert(kLightsFixedBytes == 11 && kLightBytes == 72 && kEnvironmentHintBytes == 245);

constexpr u8 kPrioKey = 255;
constexpr u8 kPrioPlayerLight = 224;
constexpr u8 kPrioPriorityPoint = 192;
constexpr u8 kPrioBossSpot = 160;
constexpr u8 kPrioPoint = 128;
constexpr u8 kPrioRoom = 112;
constexpr u8 kPrioNeighbourRoom = 104;
constexpr u8 kPrioEffect = 80;

constexpr u16 kFillFlags = AURORA_LIGHT_CAMERA_FILL | AURORA_LIGHT_NO_SHADOW |
                           AURORA_LIGHT_NO_SPECULAR | AURORA_LIGHT_NO_VOLUMETRIC;
constexpr u16 kThunderFlags = AURORA_LIGHT_NO_SHADOW | AURORA_LIGHT_NO_SPECULAR |
                              AURORA_LIGHT_NO_VOLUMETRIC | AURORA_LIGHT_FLICKER;

constexpr std::string_view kUsage = "Usage: render.lights [list|slots|check|counts|log <n>]";

struct FlagName {
    u16 bit;
    std::string_view name;
};

constexpr std::array<FlagName, 9> kFlagNames{{
    {AURORA_LIGHT_NO_SHADOW, "NO_SHADOW"},
    {AURORA_LIGHT_NO_SPECULAR, "NO_SPECULAR"},
    {AURORA_LIGHT_FLICKER, "FLICKER"},
    {AURORA_LIGHT_CAMERA_FILL, "CAMERA_FILL"},
    {AURORA_LIGHT_AFFECTS_TRANSLUCENT, "AFFECTS_TRANSLUCENT"},
    {AURORA_LIGHT_CLOUD_SHADOWED, "CLOUD_SHADOWED"},
    {AURORA_LIGHT_KEY_ROOM, "KEY_ROOM"},
    {AURORA_LIGHT_LANTERN, "LANTERN"},
    {AURORA_LIGHT_NO_VOLUMETRIC, "NO_VOLUMETRIC"},
}};

constexpr std::array<FlagName, 9> kEnvFlagNames{{
    {AURORA_ENV_INTERIOR, "INTERIOR"},
    {AURORA_ENV_TWILIGHT, "TWILIGHT"},
    {AURORA_ENV_UNDERWATER, "UNDERWATER"},
    {AURORA_ENV_SENSES, "SENSES"},
    {AURORA_ENV_RAIN, "RAIN"},
    {AURORA_ENV_SNOW, "SNOW"},
    {AURORA_ENV_NO_SUN, "NO_SUN"},
    {AURORA_ENV_NO_VOLUMETRIC, "NO_VOLUMETRIC"},
    {AURORA_ENV_ROOM, "ROOM"},
}};

// A flag added to aurora must get a name here before a listing can print it
template <size_t N>
constexpr bool names_cover_every_bit(const std::array<FlagName, N>& names, u32 top) {
    u32 seen = 0;
    for (const FlagName& flag : names) {
        if (flag.bit == 0 || (flag.bit & (flag.bit - 1)) != 0 || (seen & flag.bit) != 0) {
            return false;
        }
        seen |= flag.bit;
    }
    return seen == (top << 1) - 1;
}
static_assert(names_cover_every_bit(kFlagNames, AURORA_LIGHT_NO_VOLUMETRIC));
static_assert(names_cover_every_bit(kEnvFlagNames, AURORA_ENV_ROOM));

std::string_view kind_name(LightKind kind) noexcept {
    constexpr std::array<std::string_view, kKindCount> kNames{"None", "Sun", "Moon", "KeyRoom",
        "CameraFill", "PlayerLight", "Reserved", "BossSpot", "TwilightCamera", "WaterCamera",
        "Point", "Effect", "ThunderFlash", "Room"};
    const auto index = static_cast<size_t>(kind);
    return index < kNames.size() ? kNames[index] : "?";
}

std::string_view state_name(SlotState state) noexcept {
    constexpr std::array<std::string_view, kSlotStateCount> kNames{"Empty", "Emitted", "Disabled",
        "Duplicate", "Queued", "Replaced", "Key", "Unused", "Dropped"};
    const auto index = static_cast<size_t>(state);
    return index < kNames.size() ? kNames[index] : "?";
}

std::string names_of(u32 flags, std::span<const FlagName> names) {
    std::string text;
    for (const FlagName& flag : names) {
        if ((flags & flag.bit) != 0) {
            if (!text.empty()) {
                text += '|';
            }
            text += flag.name;
        }
    }
    return text.empty() ? std::string{"0"} : text;
}

std::string flag_names(u16 flags) {
    return names_of(flags, kFlagNames);
}

std::string env_flag_names(u32 flags) {
    return names_of(flags, kEnvFlagNames);
}

std::string_view size_name(u8 size) noexcept {
    switch (size) {
    case LIGHT_SIZE_S:
        return "S";
    case LIGHT_SIZE_M:
        return "M";
    case LIGHT_SIZE_L:
        return "L";
    case LIGHT_SIZE_LL:
        return "LL";
    default:
        return "?";
    }
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

Vec sub(const Vec& a, const Vec& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec add(const Vec& a, const Vec& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

f32 length(const Vec& v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

bool normalize(const Vec& v, Vec& out, f32 minLength) noexcept {
    const f32 len = length(v);
    if (!(len >= minLength)) {
        return false;
    }
    out = {v.x / len, v.y / len, v.z / len};
    return true;
}

Vec negate(const Vec& v) noexcept {
    return {-v.x, -v.y, -v.z};
}

// atan2 of cross and dot stays accurate near 0 degrees, where acos of the dot does not
double angle_degrees(const Vec& a, const Vec& b) noexcept {
    const double cx = double(a.y) * b.z - double(a.z) * b.y;
    const double cy = double(a.z) * b.x - double(a.x) * b.z;
    const double cz = double(a.x) * b.y - double(a.y) * b.x;
    const double d = double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), d) * (180.0 / kPi);
}

bool within(const Vec& a, const Vec& b, f32 tolerance) noexcept {
    return length(sub(a, b)) <= tolerance;
}

void put(f32 (&dst)[3], const Vec& v) noexcept {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}

Vec get(const f32 (&src)[3]) noexcept {
    return {src[0], src[1], src[2]};
}

// Recomputed from daytime, because setSunpos itself writes sun_pos and moon_pos
f32 sky_angle(f32 daytime) {
    if (daytime >= 90.0f && daytime <= 270.0f) {
        return dKy_get_parcent(270.0f, 90.0f, daytime) * 150.0f + 105.0f;
    }
    f32 angle = daytime < 90.0f ? daytime + 360.0f : daytime;
    angle = dKy_get_parcent(450.0f, 270.0f, angle) * 210.0f + 255.0f;
    return angle > 360.0f ? angle - 360.0f : angle;
}

f32 moon_daytime(f32 daytime) noexcept {
    const f32 moon = 180.0f + daytime;
    return moon >= 360.0f ? moon - 360.0f : moon;
}

Vec sky_offset(f32 angle) noexcept {
    const f32 radians = static_cast<f32>(angle * (kPi / 180.0));
    const f32 c = cosf(radians);
    return {sinf(radians) * kSkyDistance, -(c * kSkyDistance), c * kSkyDepth};
}

f32 sun_weight(f32 daytime) noexcept {
    const f32 rise = std::clamp((daytime - kSunRise) / kCrossFade, 0.0f, 1.0f);
    const f32 set = std::clamp((kSunSet - daytime) / kCrossFade, 0.0f, 1.0f);
    return std::min(rise, set);
}

// SetBaseLight's window: the body the game's own key light follows
bool sun_is_key(f32 daytime) noexcept {
    return daytime > kSunRise && daytime < kSunSet;
}

f32 linear_channel(f32 value) noexcept {
    const f32 c = std::max(value, 0.0f) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

template <typename Color>
void put_linear(f32 (&dst)[3], const Color& color, f32 scale = 1.0f) noexcept {
    dst[0] = linear_channel(static_cast<f32>(color.r)) * scale;
    dst[1] = linear_channel(static_cast<f32>(color.g)) * scale;
    dst[2] = linear_channel(static_cast<f32>(color.b)) * scale;
}

// Solved on aurora's coefficients so the radius matches what its GX lighting attenuates
f32 attn_radius(f32 ref, f32 brightness, u8 fn) noexcept {
    if (ref < 0.0f || brightness < 0.0f || brightness >= 1.0f) {
        fn = GX_DA_OFF;
    }
    if (ref <= 1e-6f) {
        return 0.0f;
    }
    f32 k1 = 0.0f;
    f32 k2 = 0.0f;
    switch (fn) {
    case GX_DA_GENTLE:
        k1 = (1.0f - brightness) / (brightness * ref);
        break;
    case GX_DA_MEDIUM:
        k1 = 0.5f * (1.0f - brightness) / (brightness * ref);
        k2 = 0.5f * (1.0f - brightness) / (brightness * ref * ref);
        break;
    case GX_DA_STEEP:
        k2 = (1.0f - brightness) / (brightness * ref * ref);
        break;
    default:
        return kMaxLightRadius;
    }
    const double a = k1;
    const double b = k2;
    const double d = b > 0.0 ? (-a + std::sqrt(a * a + 1016.0 * b)) / (2.0 * b) : 254.0 / a;
    if (!std::isfinite(d)) {
        return kMaxLightRadius;
    }
    return static_cast<f32>(std::clamp(d, 0.0, static_cast<double>(kMaxLightRadius)));
}

f32 falloff_code(u8 fn) noexcept {
    switch (fn) {
    case GX_DA_GENTLE:
        return 1.0f;
    case GX_DA_MEDIUM:
        return 1.5f;
    case GX_DA_STEEP:
        return 2.0f;
    default:
        return 0.0f;
    }
}

struct SpotShape {
    bool spot = false;
    Vec direction{};
    f32 inner = 1.0f;
    f32 outer = -1.0f;
};

// Same cutoff rule and direction as the game's spot setup, so the hint matches the GX light
SpotShape spot_from(u8 fn, f32 cutoff, f32 angleX, f32 angleY) {
    SpotShape shape;
    if (fn == GX_SP_OFF || cutoff <= 0.0f || cutoff > 90.0f) {
        return shape;
    }
    const f32 x = static_cast<f32>(angleX / (180.0 / kPi));
    const f32 y = static_cast<f32>(angleY / (180.0 / kPi));
    shape.spot = true;
    shape.direction = {cM_fcos(x) * cM_fcos(y), cM_fsin(x), cM_fcos(x) * cM_fsin(y)};
    shape.outer = cosf(static_cast<f32>((cutoff * kPi) / 180.0));
    shape.inner = fn == GX_SP_FLAT ? std::min(1.0f, shape.outer + 0.001f) : 1.0f;
    return shape;
}

AuroraLight make_light(AuroraLightType type) noexcept {
    AuroraLight light{};
    light.type = type;
    light.spotInnerCos = 1.0f;
    light.spotOuterCos = -1.0f;
    light.volumetricScale = 1.0f;
    return light;
}

void apply_spot(AuroraLight& light, const SpotShape& shape) noexcept {
    light.type = shape.spot ? AURORA_LIGHT_SPOT : AURORA_LIGHT_POINT;
    put(light.direction, shape.direction);
    light.spotInnerCos = shape.inner;
    light.spotOuterCos = shape.outer;
}

bool is_black(const AuroraLight& light) noexcept {
    return light.color[0] <= 0.0f && light.color[1] <= 0.0f && light.color[2] <= 0.0f;
}

bool is_tiny(const AuroraLight& light) noexcept {
    return light.type != AURORA_LIGHT_DIRECTIONAL && light.radius < 1.0f;
}

struct FillInputs {
    u8 type = 0;
    bool darkworld = false;
    int id = -1;
    f32 distance = 0.0f;
    f32 power = 0.0f;
    bool outdoor = false;
    bool mn09a = false;
};

// Evaluated here because settingTevStruct_plightcol_plus writes the tevstr while it decides
bool camera_fill(const FillInputs& in) noexcept {
    int id = in.id;
    const u8 t = in.type;
    if (t == 7 || t == 1 || t == 4 || t == 5 || ((t == 2 || t == 6 || t == 3) && in.darkworld) ||
        (t == 9 && in.darkworld))
    {
        id = -2;
    }
    const bool nearLight = id >= 0 && in.distance < 1000.0f + std::max(in.power, 0.001f);
    return !nearLight && (id <= -2 || (id == -1 && in.outdoor) || in.mn09a);
}

struct EnvSlotClass {
    SlotState state;
    LightKind kind;
};

EnvSlotClass classify_env_slot(int index, u8 active, u8 alpha) noexcept {
    if (index >= kEnvSlotsUsed) {
        return {SlotState::Unused, LightKind::None};
    }
    if (active != 1) {
        return {SlotState::Empty, LightKind::None};
    }
    if (alpha == 254) {
        return {SlotState::Emitted, LightKind::TwilightCamera};
    }
    if (alpha == 253) {
        return {SlotState::Emitted, LightKind::WaterCamera};
    }
    return {SlotState::Emitted, index == 0 ? LightKind::PlayerLight : LightKind::BossSpot};
}

// LANTERN follows the lantern shadow-source state, so it clears before the light fades
u16 player_light_flags(u8 shadowMode, bool flame) noexcept {
    u16 flags = AURORA_LIGHT_AFFECTS_TRANSLUCENT;
    if ((shadowMode & 2) != 0 && flame) {
        flags |= AURORA_LIGHT_LANTERN;
    }
    return flags;
}

f32 bg_lift(int index, f32 slot0Lift) noexcept {
    return index == 0 ? slot0Lift : kEnvSlotLift;
}

f32 env_slot_lift(const dKy_tevstr_c* lightTev, int index, f32 slot0Lift) noexcept {
    return lightTev != nullptr && lightTev->Type >= 16 ? bg_lift(index, slot0Lift) : 0.0f;
}

// The game declares these without const although they only read
bool light_switch_on(const stage_pure_lightvec_info_class& info, int room) {
    return dKy_lightswitch_check(
               const_cast<stage_pure_lightvec_info_class*>(&info), static_cast<char>(room)) == TRUE;
}

const cXyz* lantern_flame(const fopAc_ac_c* player) {
    if (player == nullptr) {
        return nullptr;
    }
    return const_cast<daPy_py_c*>(static_cast<const daPy_py_c*>(player))->getKandelaarFlamePos();
}

struct RoomLights {
    const stage_pure_lightvec_info_class* info = nullptr;
    int count = 0;
};

RoomLights room_lights(int room) {
    RoomLights lights;
    if (room < 0 || room >= kRoomCount) {
        return lights;
    }
    const dStage_roomDt_c* data = dComIfGp_roomControl_getStatusRoomDt(room);
    if (data != nullptr && data->getLightVecInfo() != nullptr) {
        lights.info = data->getLightVecInfo();
        lights.count = std::clamp(data->getLightVecInfoNum(), 0, kRoomLights);
    }
    return lights;
}

bool room_displayed(int room) {
    return room >= 0 && room < kRoomCount && dComIfGp_roomControl_checkStatusFlag(room, 0x10) != 0;
}

bool stage_is(const char* name) {
    return strcmp(dComIfGp_getStartStageName(), name) == 0;
}

// One sun decision for both hints: sun and moon or KEY_ROOM in the list, NO_SUN in the environment
bool sun_moon_lit() {
    return dKy_SunMoon_Light_Check() == TRUE;
}

struct StageKey {
    std::array<char, 8> name{};
    s32 layer = -1;
    u32 epoch = 0;
    bool operator==(const StageKey&) const = default;
};

// Each stage create runs a new play scene, so even a reload of the same stage is a new epoch
struct LoadTracker {
    bool seen = false;
    fpc_ProcID scene = 0;
    u32 epoch = 0;
    s32 layer = -1;
};

StageKey current_stage_key(LoadTracker& loads) {
    const fpc_ProcID scene = dStage_roomControl_c::getProcID();
    if (!loads.seen || loads.scene != scene) {
        loads.seen = true;
        loads.scene = scene;
        ++loads.epoch;
        // Latched at the load, so a day/night layer flip inside one stage keeps every id
        loads.layer = dComIfG_play_c::getLayerNo(0);
    }
    StageKey key;
    if (const char* name = dComIfGp_getStartStageName(); name != nullptr) {
        for (size_t i = 0; i < key.name.size() && name[i] != '\0'; ++i) {
            key.name[i] = name[i];
        }
    }
    key.layer = loads.layer;
    key.epoch = loads.epoch;
    return key;
}

std::string stage_text(const StageKey& key) {
    const auto end = std::find(key.name.begin(), key.name.end(), '\0');
    return fmt::format("{}/{} load {}", std::string_view(key.name.data(), end - key.name.begin()),
        key.layer, key.epoch);
}

constexpr u16 next_generation(u16 generation) noexcept {
    return generation == 0xFFFF ? 1 : static_cast<u16>(generation + 1);
}

constexpr u32 make_light_id(LightKind kind, u32 slot, u16 generation) noexcept {
    return static_cast<u32>(kind) << 26 | slot << 16 | generation;
}

static_assert(make_light_id(LightKind::PlayerLight, 0, 2) == 0x14000002);
static_assert(make_light_id(LightKind::Point, 12, 1) == 0x280C0001);
static_assert(make_light_id(LightKind::Room, kIdSlots - 1, 0xFFFF) >> 26 == 13);

u16 id_generation(u32 id) noexcept {
    return static_cast<u16>(id & 0xFFFF);
}

class LightIds {
public:
    void begin_build(const StageKey& stage) noexcept {
        // A new stage key (another stage, layer or load) skips a build number, so no id survives it
        m_build += m_stage == stage ? 1 : 2;
        m_stage = stage;
    }

    u32 assign(LightKind kind, u32 slot, const void* key) noexcept {
        Entry& entry = m_entries[static_cast<size_t>(kind)][slot];
        if (entry.generation == 0 || entry.seen + 1 != m_build || entry.key != key) {
            entry.generation = next_generation(entry.generation);
        }
        entry.seen = m_build;
        entry.key = key;
        return make_light_id(kind, slot, entry.generation);
    }

    u32 builds() const noexcept { return m_build; }

private:
    struct Entry {
        const void* key = nullptr;
        u32 seen = 0;
        u16 generation = 0;
    };

    std::array<std::array<Entry, kIdSlots>, kKindCount> m_entries{};
    u32 m_build = 0;
    StageKey m_stage{};
};

struct LightNote {
    LightKind kind = LightKind::None;
    u16 slot = 0;
    s8 room = -1;
    bool neighbour = false;
    f32 ref = 0.0f;
    u8 fn = GX_DA_OFF;
    f32 weight = 1.0f;
};

struct LightList {
    std::array<AuroraLight, AURORA_MAX_LIGHTS> lights{};
    std::array<LightNote, AURORA_MAX_LIGHTS> notes{};
    u16 count = 0;
    u32 dropped = 0;

    // Lights past the limit go from the end of the fixed order
    bool add(const AuroraLight& light, const LightNote& note) noexcept {
        if (count >= lights.size()) {
            ++dropped;
            return false;
        }
        lights[count] = light;
        notes[count] = note;
        ++count;
        return true;
    }
};

// Equal ranks sort by slot, and room lights put the light room before its neighbours
int kind_rank(LightKind kind) noexcept {
    switch (kind) {
    case LightKind::Sun:
        return 0;
    case LightKind::Moon:
        return 1;
    case LightKind::KeyRoom:
        return 2;
    case LightKind::CameraFill:
        return 3;
    case LightKind::PlayerLight:
    case LightKind::BossSpot:
        return 4;
    case LightKind::TwilightCamera:
    case LightKind::WaterCamera:
        return 5;
    case LightKind::Point:
        return 6;
    case LightKind::Effect:
    case LightKind::ThunderFlash:
        return 7;
    case LightKind::Room:
        return 8;
    default:
        return 9;
    }
}

std::string order_problems(const LightList& list) {
    std::string problems;
    const auto rank = [&list](size_t i) {
        const LightNote& note = list.notes[i];
        return std::make_tuple(kind_rank(note.kind), note.neighbour, note.room, note.slot);
    };
    for (size_t i = 1; i < list.count; ++i) {
        if (!(rank(i - 1) < rank(i))) {
            append_problem(problems,
                fmt::format("#{} {} slot {} after {} slot {}", i, kind_name(list.notes[i].kind),
                    list.notes[i].slot, kind_name(list.notes[i - 1].kind), list.notes[i - 1].slot));
            break;
        }
    }
    return problems;
}

struct SlotEntry {
    SlotState state = SlotState::Empty;
    u32 id = 0;
    f32 value = 0.0f;
    s32 index = 0;
};

struct SlotReport {
    std::array<SlotEntry, kPointCount> point{};
    std::array<SlotEntry, kEffectCount> effect{};
    std::array<SlotEntry, kDungeonCount> dungeon{};
    std::array<SlotEntry, kEnvSlotCount> envSlot{};
    std::array<SlotEntry, kQueuedCount> queued{};
};

struct SceneLights {
    LightList list;
    SlotReport slots;
    std::array<std::array<SlotEntry, kRoomLights>, kRoomCount> rooms{};
    LightKind key = LightKind::None;
    int lightRoom = -1;
    StageKey stage;
    u32 frame = 0;
    bool sunMoon = false;
    bool slot0 = false;
    Vec slot0Pos{};
    bool flame = false;
    Vec flamePos{};

    void reset() noexcept {
        list.count = 0;
        list.dropped = 0;
        slots = {};
        rooms = {};
        key = LightKind::None;
        lightRoom = -1;
        frame = 0;
        sunMoon = false;
        slot0 = false;
        flame = false;
    }
};

struct LightState {
    SceneLights lights;
    LightIds ids;
    LoadTracker loads;
};

using RoomMask = std::array<bool, kRoomCount>;

// The light room comes first, so the neighbour rooms are the first lights dropped
int room_order(int lightRoom, const RoomMask& displayed, std::array<s8, kRoomCount>& order) {
    int count = 0;
    if (lightRoom >= 0 && lightRoom < kRoomCount && displayed[lightRoom]) {
        order[count++] = static_cast<s8>(lightRoom);
    }
    for (int room = 0; room < kRoomCount; ++room) {
        if (room != lightRoom && displayed[room]) {
            order[count++] = static_cast<s8>(room);
        }
    }
    return count;
}

struct EnvSlotInputs {
    const BOSS_LIGHT* slots = nullptr;
    const BOSS_LIGHT* queued = nullptr;
    u8 shadowMode = 0;
    f32 slot0Lift = 0.0f;
};

EnvSlotInputs live_env_slots() noexcept {
    return {g_env_light.field_0x0c18, g_env_light.field_0x0d58, g_env_light.shadow_mode,
        g_env_light.field_0x127c};
}

struct BuildContext {
    SceneLights& out;
    LightIds& ids;
    const camera_process_class* camera = nullptr;
    const fopAc_ac_c* player = nullptr;
    const dKy_tevstr_c* lightTev = nullptr;
    RoomLights lightInfo;
    int lightRoom = -1;
    bool sunMoon = false;
    bool keyRoom = false;
};

SlotState emit(BuildContext& ctx, AuroraLight light, const LightNote& note, const void* key,
    SlotEntry* entry = nullptr) {
    light.id = ctx.ids.assign(note.kind, note.slot, key);
    const SlotState state = ctx.out.list.add(light, note) ? SlotState::Emitted : SlotState::Dropped;
    if (entry != nullptr) {
        entry->state = state;
        entry->id = light.id;
    }
    return state;
}

Vec camera_eye(const BuildContext& ctx) noexcept {
    return ctx.camera != nullptr ? Vec(ctx.camera->view.lookat.eye) : Vec{0.0f, 0.0f, 0.0f};
}

void add_sky_body(
    BuildContext& ctx, LightKind kind, f32 angle, f32 weight, bool key, const J3DLightObj& source) {
    if (weight <= 0.0f) {
        return;
    }
    const Vec offset = sky_offset(angle);
    Vec toBody;
    if (!normalize(offset, toBody, 1.0f)) {
        return;
    }
    AuroraLight light = make_light(AURORA_LIGHT_DIRECTIONAL);
    put(light.position, add(camera_eye(ctx), offset));
    put(light.direction, negate(toBody));
    put_linear(light.color, source.mInfo.mColor, weight);
    if (is_black(light)) {
        return;
    }
    light.flags = AURORA_LIGHT_CLOUD_SHADOWED | AURORA_LIGHT_AFFECTS_TRANSLUCENT;
    if (!key) {
        light.flags |= AURORA_LIGHT_NO_SHADOW;
    }
    light.shadowPriority = key ? kPrioKey : 0;
    LightNote note;
    note.kind = kind;
    note.weight = weight;
    if (emit(ctx, light, note, nullptr) == SlotState::Emitted && key) {
        ctx.out.key = kind;
    }
}

void add_sky(BuildContext& ctx) {
    if (!ctx.sunMoon || ctx.lightTev == nullptr) {
        return;
    }
    const f32 daytime = g_env_light.daytime;
    const f32 weight = sun_weight(daytime);
    const bool sunKey = sun_is_key(daytime);
    add_sky_body(ctx, LightKind::Sun, sky_angle(daytime), weight, sunKey, ctx.lightTev->mLights[0]);
    add_sky_body(ctx, LightKind::Moon, sky_angle(moon_daytime(daytime)), 1.0f - weight, !sunKey,
        ctx.lightTev->mLights[1]);
}

// Points from the light room's LGT0 toward the player, the source of the player's real shadow
void add_key_room(BuildContext& ctx) {
    if (ctx.sunMoon || ctx.lightTev == nullptr || ctx.lightInfo.count < 1 ||
        !light_switch_on(ctx.lightInfo.info[0], ctx.lightRoom))
    {
        return;
    }
    const Vec position = ctx.lightInfo.info[0].position;
    Vec direction{0.0f, -1.0f, 0.0f};
    if (ctx.player != nullptr) {
        normalize(sub(ctx.player->current.pos, position), direction, 1.0f);
    } else if (ctx.camera != nullptr) {
        normalize(sub(ctx.camera->view.lookat.center, position), direction, 1.0f);
    }
    AuroraLight light = make_light(AURORA_LIGHT_DIRECTIONAL);
    put(light.position, position);
    put(light.direction, direction);
    put_linear(light.color, ctx.lightTev->mLights[0].mInfo.mColor);
    if (is_black(light)) {
        return;
    }
    light.flags = AURORA_LIGHT_KEY_ROOM | AURORA_LIGHT_AFFECTS_TRANSLUCENT;
    light.shadowPriority = kPrioKey;
    LightNote note;
    note.kind = LightKind::KeyRoom;
    note.slot = static_cast<u16>(ctx.lightRoom);
    note.room = static_cast<s8>(ctx.lightRoom);
    if (emit(ctx, light, note, nullptr) == SlotState::Emitted) {
        ctx.out.key = LightKind::KeyRoom;
        ctx.keyRoom = true;
    }
}

void add_camera_fill(BuildContext& ctx) {
    if (ctx.player == nullptr) {
        return;
    }
    const cXyz& pos = ctx.player->current.pos;
    const dKy_tevstr_c& tev = ctx.player->tevStr;
    FillInputs in;
    in.type = tev.Type;
    in.darkworld = dKy_darkworld_check() != 0;
    in.id = dKy_light_influence_id(pos, 0);
    in.distance = 1e30f;
    if (in.id >= 0 && in.id < kPointCount && g_env_light.pointlight[in.id] != nullptr) {
        const LIGHT_INFLUENCE& nearest = *g_env_light.pointlight[in.id];
        in.distance = length(sub(pos, nearest.mPosition));
        in.power = nearest.mPow;
    }
    in.outdoor = dKy_Outdoor_check() == TRUE;
    in.mn09a = stage_is("D_MN09A");
    if (!camera_fill(in)) {
        return;
    }
    Vec direction{0.0f, -1.0f, 0.0f};
    if (!normalize(sub(pos, tev.field_0x32c), direction, 1.0f) && ctx.camera != nullptr) {
        normalize(
            sub(ctx.camera->view.lookat.center, ctx.camera->view.lookat.eye), direction, 1e-6f);
    }
    AuroraLight light = make_light(AURORA_LIGHT_DIRECTIONAL);
    put(light.position, tev.field_0x32c);
    put(light.direction, direction);
    put_linear(light.color, tev.mLightObj.mInfo.mColor);
    if (is_black(light)) {
        return;
    }
    light.flags = kFillFlags;
    light.categoryMask = kCameraFillMask;
    light.volumetricScale = 0.0f;
    LightNote note;
    note.kind = LightKind::CameraFill;
    emit(ctx, light, note, nullptr);
}

void add_env_slot(BuildContext& ctx, const EnvSlotInputs& in, int index, LightKind kind) {
    const BOSS_LIGHT& b = in.slots[index];
    SlotEntry& entry = ctx.out.slots.envSlot[index];
    AuroraLight light = make_light(AURORA_LIGHT_POINT);
    apply_spot(light, spot_from(b.mAngleAttenuation, b.mCutoffAngle, b.mAngleX, b.mAngleY));
    put(light.position, b.mPos);
    put_linear(light.color, b.mColor);
    light.radius = attn_radius(b.mRefDistance, kRefBrightness, b.mDistAttenuation);
    light.falloffExponent = falloff_code(b.mDistAttenuation);
    switch (kind) {
    case LightKind::PlayerLight:
        light.flags = player_light_flags(in.shadowMode, ctx.out.flame);
        light.shadowPriority = kPrioPlayerLight;
        ctx.out.slot0 = true;
        ctx.out.slot0Pos = b.mPos;
        break;
    case LightKind::BossSpot:
        light.flags = AURORA_LIGHT_AFFECTS_TRANSLUCENT;
        light.shadowPriority = kPrioBossSpot;
        break;
    default:
        light.flags = kFillFlags;
        light.volumetricScale = 0.0f;
        break;
    }
    if (is_black(light) || is_tiny(light)) {
        entry.state = SlotState::Disabled;
        return;
    }
    LightNote note;
    note.kind = kind;
    note.slot = static_cast<u16>(index);
    note.ref = b.mRefDistance;
    note.fn = b.mDistAttenuation;
    emit(ctx, light, note, nullptr, &entry);
}

void add_env_slots(BuildContext& ctx, const EnvSlotInputs& in) {
    std::array<LightKind, kEnvSlotCount> kinds{};
    for (int i = 0; i < kEnvSlotCount; ++i) {
        const BOSS_LIGHT& b = in.slots[i];
        const EnvSlotClass slot = classify_env_slot(i, b.field_0x26, b.mColor.a);
        SlotEntry& entry = ctx.out.slots.envSlot[i];
        entry.state = slot.state;
        entry.value =
            slot.state == SlotState::Unused ? 0.0f : env_slot_lift(ctx.lightTev, i, in.slot0Lift);
        kinds[i] = slot.kind;
    }
    for (int i = 0; i < kEnvSlotsUsed; ++i) {
        if (kinds[i] == LightKind::PlayerLight || kinds[i] == LightKind::BossSpot) {
            add_env_slot(ctx, in, i, kinds[i]);
        }
    }
    for (int i = 0; i < kEnvSlotsUsed; ++i) {
        if (kinds[i] == LightKind::TwilightCamera || kinds[i] == LightKind::WaterCamera) {
            add_env_slot(ctx, in, i, kinds[i]);
        }
    }
    for (int i = 0; i < kQueuedCount; ++i) {
        ctx.out.slots.queued[i].state =
            in.queued[i].field_0x26 != 0 ? SlotState::Queued : SlotState::Empty;
    }
}

bool listed(const LIGHT_INFLUENCE* const* lights, int count, const LIGHT_INFLUENCE* light) {
    return std::find(lights, lights + count, light) != lights + count;
}

// The shared part of the point and effect slots; false leaves the slot's state final
bool influence_candidate(const LIGHT_INFLUENCE* p, bool duplicate, f32 refScale, f32 radiusScale,
    SlotEntry& entry, AuroraLight& light, LightNote& note) {
    if (p == nullptr) {
        return false;
    }
    entry.value = p->mPow;
    entry.index = p->mIndex;
    if (p->mPow <= 0.01f) {
        entry.state = SlotState::Disabled;
        return false;
    }
    if (duplicate) {
        entry.state = SlotState::Duplicate;
        return false;
    }
    note.ref = p->mPow * refScale;
    note.fn = GX_DA_STEEP;
    light = make_light(AURORA_LIGHT_POINT);
    put(light.position, p->mPosition);
    put_linear(light.color, p->mColor);
    light.radius = radiusScale * attn_radius(note.ref, kRefBrightness, GX_DA_STEEP);
    light.falloffExponent = falloff_code(GX_DA_STEEP);
    if (is_black(light) || is_tiny(light)) {
        entry.state = SlotState::Disabled;
        return false;
    }
    light.flags = AURORA_LIGHT_AFFECTS_TRANSLUCENT;
    if (p->mFluctuation > 0.0f) {
        light.flags |= AURORA_LIGHT_FLICKER;
    }
    return true;
}

void add_point_lights(BuildContext& ctx) {
    const LIGHT_INFLUENCE* const* lights = g_env_light.pointlight;
    for (int i = 0; i < kPointCount; ++i) {
        const LIGHT_INFLUENCE* p = lights[i];
        SlotEntry& entry = ctx.out.slots.point[i];
        AuroraLight light{};
        LightNote note;
        if (!influence_candidate(
                p, listed(lights, i, p), 0.001f, kPointRadiusScale, entry, light, note))
        {
            continue;
        }
        // A negative index is the priority path, which also carries the 0x8000 bit
        light.shadowPriority = p->mIndex < 0 ? kPrioPriorityPoint : kPrioPoint;
        note.kind = LightKind::Point;
        note.slot = static_cast<u16>(i);
        emit(ctx, light, note, p, &entry);
    }
}

void add_effect_lights(BuildContext& ctx) {
    const LIGHT_INFLUENCE* const* lights = g_env_light.efplight;
    const LIGHT_INFLUENCE* thunder = &g_env_light.mThunderEff.mLightInfluence;
    for (int i = 0; i < kEffectCount; ++i) {
        const LIGHT_INFLUENCE* p = lights[i];
        SlotEntry& entry = ctx.out.slots.effect[i];
        const bool duplicate = p != nullptr && (listed(g_env_light.pointlight, kPointCount, p) ||
                                                   listed(lights, i, p));
        AuroraLight light{};
        LightNote note;
        if (!influence_candidate(p, duplicate, 0.01f, 1.0f, entry, light, note)) {
            continue;
        }
        note.slot = static_cast<u16>(i);
        if (p == thunder) {
            note.kind = LightKind::ThunderFlash;
            light.flags = kThunderFlags;
            light.volumetricScale = 0.0f;
        } else {
            note.kind = LightKind::Effect;
            light.shadowPriority = kPrioEffect;
            light.volumetricScale = 0.5f;
        }
        emit(ctx, light, note, p, &entry);
    }
}

void add_room(BuildContext& ctx, int room) {
    const RoomLights lights = room_lights(room);
    if (lights.count == 0) {
        return;
    }
    const dKy_tevstr_c* tev = dComIfGp_roomControl_getTevStr(room);
    const bool neighbour = room != ctx.lightRoom;
    for (int i = 0; i < lights.count; ++i) {
        SlotEntry& entry = ctx.out.rooms[room][i];
        if (ctx.sunMoon && i <= 1) {
            entry.state = SlotState::Replaced;
            continue;
        }
        if (ctx.keyRoom && !neighbour && i == 0) {
            entry.state = SlotState::Key;
            continue;
        }
        const stage_pure_lightvec_info_class& lgt = lights.info[i];
        AuroraLight light = make_light(AURORA_LIGHT_POINT);
        apply_spot(light, spot_from(lgt.spot_type, lgt.spotCutoff, lgt.directionX, lgt.directionY));
        put(light.position, lgt.position);
        put_linear(light.color, tev->mLights[i].mInfo.mColor);
        light.radius = attn_radius(lgt.radius, kRefBrightness, lgt.dist_atten_type);
        light.falloffExponent = falloff_code(lgt.dist_atten_type);
        if (!light_switch_on(lgt, room) || is_tiny(light) || is_black(light)) {
            entry.state = SlotState::Disabled;
            continue;
        }
        light.flags = AURORA_LIGHT_AFFECTS_TRANSLUCENT;
        light.shadowPriority = neighbour ? kPrioNeighbourRoom : kPrioRoom;
        LightNote note;
        note.kind = LightKind::Room;
        note.slot = static_cast<u16>(room * kRoomLights + i);
        note.room = static_cast<s8>(room);
        note.neighbour = neighbour;
        note.ref = lgt.radius;
        note.fn = lgt.dist_atten_type;
        emit(ctx, light, note, nullptr, &entry);
    }
}

void add_room_lights(BuildContext& ctx) {
    RoomMask displayed{};
    for (int room = 0; room < kRoomCount; ++room) {
        displayed[room] = room_displayed(room);
    }
    std::array<s8, kRoomCount> order{};
    const int count = room_order(ctx.lightRoom, displayed, order);
    for (int i = 0; i < count; ++i) {
        add_room(ctx, order[i]);
    }
    // dungeonlight[] is a stale mirror of the stay room, so its report mirrors that room too
    const int stay = dComIfGp_roomControl_getStayNo();
    for (int i = 0; i < kDungeonCount; ++i) {
        SlotEntry& entry = ctx.out.slots.dungeon[i];
        if (i >= kRoomLights) {
            entry.state = SlotState::Unused;
        } else if (stay >= 0 && stay < kRoomCount) {
            entry = ctx.out.rooms[stay][i];
        }
    }
}

void build_scene_lights(LightState& state) {
    SceneLights& out = state.lights;
    out.reset();
    out.stage = current_stage_key(state.loads);
    out.frame = aurora::gfx::current_frame();
    state.ids.begin_build(out.stage);

    BuildContext ctx{out, state.ids};
    ctx.camera = dComIfGp_getCamera(0);
    ctx.player = dComIfGp_getPlayer(0);
    const int stay = dComIfGp_roomControl_getStayNo();
    ctx.lightRoom = g_env_light.PrevCol < kRoomCount ? g_env_light.PrevCol : stay;
    if (ctx.lightRoom >= 0 && ctx.lightRoom < kRoomCount) {
        ctx.lightTev = dComIfGp_roomControl_getTevStr(ctx.lightRoom);
        ctx.lightInfo = room_lights(ctx.lightRoom);
    } else {
        ctx.lightRoom = -1;
    }
    out.lightRoom = ctx.lightRoom;
    ctx.sunMoon = sun_moon_lit();
    out.sunMoon = ctx.sunMoon;
    if (const cXyz* flame = lantern_flame(ctx.player)) {
        out.flame = true;
        out.flamePos = *flame;
    }

    add_sky(ctx);
    add_key_room(ctx);
    add_camera_fill(ctx);
    add_env_slots(ctx, live_env_slots());
    add_point_lights(ctx);
    add_effect_lights(ctx);
    add_room_lights(ctx);
}

struct SubmitSeam {
    bool (*gate)() noexcept;
    void (*build)(LightState& state);
    void (*emit)(const AuroraLight* lights, u16 count);
};

constexpr SubmitSeam kLiveSeam{&has_main_camera, &build_scene_lights, &GXAuroraSetLights};

bool submit_through(const SubmitSeam& seam, LightState& state) {
    if (!seam.gate()) {
        return false;
    }
    seam.build(state);
    seam.emit(state.lights.list.lights.data(), state.lights.list.count);
    return true;
}

struct SubmitStats {
    u64 submits = 0;
    u64 lights = 0;
    u64 bytes = 0;
    u64 dropped = 0;
    u16 maxCount = 0;
    bool any = false;
    u32 lastFrame = 0;
};

LightState s_live;
SubmitStats s_stats;
u32 s_logRemaining = 0;
StageKey s_droppedStage;
bool s_warnedDropped = false;

struct CameraPainter {
    bool seen = false;
    u32 frame = 0;
    Vec sunLightPos{};
    Vec previousSunLightPos{};
};

CameraPainter s_cameraPainter;

// dKy_setLight runs just before this in the same camera block, so this sees every sun_light_pos
void note_camera_painter() noexcept {
    const Vec copied = g_env_light.sun_light_pos;
    s_cameraPainter.previousSunLightPos =
        s_cameraPainter.seen ? s_cameraPainter.sunLightPos : copied;
    s_cameraPainter.sunLightPos = copied;
    s_cameraPainter.seen = true;
    s_cameraPainter.frame = aurora::gfx::current_frame();
}

// sun_light_pos and the painter's GX light programming only move in a painter with a camera block
bool last_painter_had_camera() noexcept {
    return s_cameraPainter.seen && s_cameraPainter.frame + 1 == aurora::gfx::current_frame();
}

// Copies the live ids and load epoch so a check never moves what the game's own frames see
std::unique_ptr<LightState> make_scratch() {
    auto scratch = std::make_unique<LightState>();
    scratch->ids = s_live.ids;
    scratch->loads = s_live.loads;
    return scratch;
}

std::string light_line(size_t index, const AuroraLight& light, const LightNote& note) {
    constexpr std::array<std::string_view, 3> kTypes{"dir", "point", "spot"};
    std::string line = fmt::format(
        "#{} {} id=0x{:08X} {} col=({:.3f},{:.3f},{:.3f}) pos=({:.1f},{:.1f},{:.1f}) "
        "dir=({:.3f},{:.3f},{:.3f}) radius={:.1f} fexp={:g} ref={:g} fn={} size={} "
        "spot=({:g},{:g}) prio={} vol={:g} flags={}",
        index, kind_name(note.kind), light.id,
        light.type < kTypes.size() ? kTypes[light.type] : "?", light.color[0], light.color[1],
        light.color[2], light.position[0], light.position[1], light.position[2], light.direction[0],
        light.direction[1], light.direction[2], light.radius, light.falloffExponent, note.ref,
        note.fn, size_name(g_env_light.light_size), light.spotInnerCos, light.spotOuterCos,
        light.shadowPriority, light.volumetricScale, flag_names(light.flags));
    if (note.kind == LightKind::Sun || note.kind == LightKind::Moon) {
        line += fmt::format(" weight={:.2f}", note.weight);
    }
    if (note.kind == LightKind::Room) {
        line += fmt::format(" room={} nb={}", static_cast<int>(note.room), note.neighbour ? 1 : 0);
    }
    if (light.categoryMask != 0) {
        line += fmt::format(" mask=0x{:X}", light.categoryMask);
    }
    return line;
}

bool list_available() noexcept {
    return has_main_camera() && s_stats.any;
}

void describe_list(std::vector<std::string>& out) {
    if (!list_available()) {
        out.emplace_back("render.lights: no list (hints off or no main camera)");
        return;
    }
    const SceneLights& lights = s_live.lights;
    for (size_t i = 0; i < lights.list.count; ++i) {
        out.push_back(light_line(i, lights.list.lights[i], lights.list.notes[i]));
    }
    out.push_back(fmt::format(
        "render.lights: {} lights, dropped {}, key={}, lightRoom={}, stage={}, frame {}",
        lights.list.count, lights.list.dropped, kind_name(lights.key), lights.lightRoom,
        stage_text(lights.stage), lights.frame));
}

struct SlotTable {
    SlotTableId id;
    std::string_view name;
    std::span<const SlotEntry> entries;
};

std::array<SlotTable, 5> slot_tables(const SlotReport& report) {
    return {{{SlotTableId::Point, "pointlight", report.point},
        {SlotTableId::Effect, "efplight", report.effect},
        {SlotTableId::Dungeon, "dungeon", report.dungeon},
        {SlotTableId::EnvSlot, "envSlot", report.envSlot},
        {SlotTableId::Queued, "queued", report.queued}}};
}

std::string slot_line(const SlotTable& table, size_t index) {
    const SlotEntry& entry = table.entries[index];
    std::string line = fmt::format("{}[{}] {}", table.name, index, state_name(entry.state));
    if (entry.id != 0) {
        line += fmt::format(" id=0x{:08X}", entry.id);
    }
    if (table.id == SlotTableId::Point || table.id == SlotTableId::Effect) {
        line += fmt::format(" mPow={:g} mIndex={}", entry.value, entry.index);
    } else if (table.id == SlotTableId::EnvSlot) {
        line += fmt::format(" lift={:g}", entry.value);
    }
    return line;
}

void describe_slots(std::vector<std::string>& out) {
    if (!list_available()) {
        out.emplace_back("render.lights: no slots (hints off or no main camera)");
        return;
    }
    size_t total = 0;
    for (const SlotTable& table : slot_tables(s_live.lights.slots)) {
        std::array<size_t, kSlotStateCount> counts{};
        for (const SlotEntry& entry : table.entries) {
            ++counts[static_cast<size_t>(entry.state)];
        }
        total += table.entries.size();
        out.push_back(fmt::format("{}: emitted {}, empty {}, disabled {}, duplicate {}, queued {}, "
                                  "replaced {}, key {}, unused {}, dropped {}",
            table.name, counts[1], counts[0], counts[2], counts[3], counts[4], counts[5], counts[6],
            counts[7], counts[8]));
    }
    for (const SlotTable& table : slot_tables(s_live.lights.slots)) {
        for (size_t i = 0; i < table.entries.size(); ++i) {
            const SlotState state = table.entries[i].state;
            if (state != SlotState::Empty && state != SlotState::Unused) {
                out.push_back(slot_line(table, i));
            }
        }
    }
    out.push_back(fmt::format("render.lights: {} slots, frame {}", total, s_live.lights.frame));
}

enum class Verdict : u8 { Pass, Fail, Skip };

struct CheckResult {
    Verdict verdict = Verdict::Pass;
    std::string detail;
};

CheckResult skip(std::string detail) {
    return {Verdict::Skip, std::move(detail)};
}

CheckResult verdict(const std::string& problems, std::string detail) {
    if (!problems.empty()) {
        return {Verdict::Fail, problems + " (" + detail + ")"};
    }
    return {Verdict::Pass, std::move(detail)};
}

size_t find_kind(const LightList& list, LightKind kind) {
    for (size_t i = 0; i < list.count; ++i) {
        if (list.notes[i].kind == kind) {
            return i;
        }
    }
    return list.count;
}

bool body_black(int lightRoom, int body) {
    if (lightRoom < 0 || lightRoom >= kRoomCount) {
        return true;
    }
    const GXColor& color = dComIfGp_roomControl_getTevStr(lightRoom)->mLights[body].mInfo.mColor;
    return color.r == 0 && color.g == 0 && color.b == 0;
}

CheckResult check_sun_matches_game() {
    if (dKy_SunMoon_Light_Check() != TRUE) {
        return skip("the SunMoon check fails in this stage");
    }
    if (stage_is("F_SP200")) {
        return skip("setSunpos skips F_SP200");
    }
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return skip("no camera 0");
    }
    const f32 daytime = g_env_light.daytime;
    Vec gameBody[2];
    if (!normalize(sub(g_env_light.sun_pos, camera->view.lookat.eye), gameBody[0], 1.0f) ||
        !normalize(g_env_light.moon_pos, gameBody[1], 1.0f))
    {
        return {Verdict::Fail, "sun_pos or moon_pos has no length"};
    }
    const auto scratch = make_scratch();
    build_scene_lights(*scratch);
    const LightList& list = scratch->lights.list;
    const LightKind kinds[2] = {LightKind::Sun, LightKind::Moon};
    const f32 weights[2] = {sun_weight(daytime), 1.0f - sun_weight(daytime)};
    std::string problems;
    std::string detail;
    for (int body = 0; body < 2; ++body) {
        const size_t index = find_kind(list, kinds[body]);
        if (index == list.count) {
            if (weights[body] > 0.0f && !body_black(scratch->lights.lightRoom, body)) {
                append_problem(problems, fmt::format("{} missing", kind_name(kinds[body])));
            }
            detail +=
                fmt::format("{}{} not listed", detail.empty() ? "" : ", ", kind_name(kinds[body]));
            continue;
        }
        const double degrees =
            angle_degrees(negate(get(list.lights[index].direction)), gameBody[body]);
        if (degrees > 1.0) {
            append_problem(problems, fmt::format("{} {:.4f} deg", kind_name(kinds[body]), degrees));
        }
        detail += fmt::format(
            "{}{} {:.4f} deg", detail.empty() ? "" : ", ", kind_name(kinds[body]), degrees);
    }
    return verdict(problems, fmt::format("{}, daytime {:.2f}", detail, daytime));
}

// The game can leave its key at an env slot, lifted or not, or at another room's LGT0
std::string base_light_source(const Vec& base) {
    for (int room = 0; room < kRoomCount; ++room) {
        const RoomLights lights = room_lights(room);
        if (room_displayed(room) && lights.count > 0 && within(base, lights.info[0].position, 1.0f))
        {
            return fmt::format("room {} LGT0", room);
        }
    }
    for (int i = 0; i < kEnvSlotsUsed; ++i) {
        const BOSS_LIGHT& b = g_env_light.field_0x0c18[i];
        if (b.field_0x26 != 1) {
            continue;
        }
        const Vec lifted{b.mPos.x, b.mPos.y + bg_lift(i, g_env_light.field_0x127c), b.mPos.z};
        if (within(base, b.mPos, 1.0f) || within(base, lifted, 1.0f)) {
            return fmt::format("envSlot[{}]{}", i, within(base, b.mPos, 1.0f) ? "" : " lifted");
        }
    }
    return {};
}

std::string key_values(const SceneLights& lights, const Vec& base, const Vec& eye) {
    std::string text = fmt::format("base_light=({:.1f},{:.1f},{:.1f}) eye=({:.1f},{:.1f},{:.1f})",
        base.x, base.y, base.z, eye.x, eye.y, eye.z);
    const LightList& list = lights.list;
    if (list.count != 0) {
        const AuroraLight& first = list.lights[0];
        text += fmt::format(" list[0]={} prio={} dir=({:.4f},{:.4f},{:.4f})",
            kind_name(list.notes[0].kind), first.shadowPriority, first.direction[0],
            first.direction[1], first.direction[2]);
    }
    text += fmt::format(" lightRoom={}", lights.lightRoom);
    const RoomLights room = room_lights(lights.lightRoom);
    if (room.count > 0) {
        const Vec lgt0 = room.info[0].position;
        text += fmt::format(" slot0={} LGT0=({:.1f},{:.1f},{:.1f})",
            state_name(lights.rooms[lights.lightRoom][0].state), lgt0.x, lgt0.y, lgt0.z);
    }
    return text;
}

CheckResult check_key_matches_base_light() {
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return skip("no camera 0");
    }
    const auto scratch = make_scratch();
    build_scene_lights(*scratch);
    const SceneLights& lights = scratch->lights;
    const LightList& list = lights.list;
    const Vec base = g_env_light.base_light.mPosition;
    const Vec eye = camera->view.lookat.eye;
    const RoomLights room = room_lights(lights.lightRoom);
    std::string problems;
    const auto finish = [&](std::string detail) {
        if (!problems.empty()) {
            detail += "; " + key_values(lights, base, eye);
        }
        return verdict(problems, std::move(detail));
    };
    for (size_t i = 1; i < list.count; ++i) {
        if (list.lights[i].shadowPriority == kPrioKey) {
            append_problem(problems,
                fmt::format("#{} {} also has the key priority", i, kind_name(list.notes[i].kind)));
        }
    }
    const bool keyFirst = list.count != 0 && list.notes[0].kind == lights.key &&
                          list.lights[0].shadowPriority == kPrioKey;
    if (lights.key == LightKind::None && list.count != 0 &&
        list.lights[0].shadowPriority == kPrioKey)
    {
        append_problem(problems, "a key priority light with no key");
    }
    if (dKy_SunMoon_Light_Check() == TRUE) {
        const bool sunKey = sun_is_key(g_env_light.daytime);
        if (lights.key == LightKind::None) {
            if (!body_black(lights.lightRoom, sunKey ? 0 : 1)) {
                append_problem(problems, "no key although the key body is lit");
            }
            return finish("no key: the key body is black");
        }
        if (!keyFirst) {
            append_problem(problems,
                fmt::format("list[0] is not the {} key with priority 255", kind_name(lights.key)));
            return finish("key check");
        }
        if (lights.key != (sunKey ? LightKind::Sun : LightKind::Moon)) {
            append_problem(
                problems, fmt::format("key {} outside its window", kind_name(lights.key)));
        }
        Vec toBody;
        Vec toBase;
        const Vec body = sunKey ? sub(g_env_light.sun_pos, eye) : Vec(g_env_light.moon_pos);
        if (!normalize(body, toBody, 1.0f) || !normalize(sub(base, eye), toBase, 1.0f)) {
            append_problem(problems, "the key body or base_light sits on the eye");
            return finish("key check");
        }
        const Vec keyToBody = negate(get(list.lights[0].direction));
        const double bodyDegrees = angle_degrees(keyToBody, toBody);
        if (bodyDegrees > 1.0) {
            append_problem(problems, fmt::format("{:.4f} deg from {}", bodyDegrees,
                                         sunKey ? "sun_pos" : "eye + moon_pos"));
        }
        // A coupled painter copies before SetBaseLight reads; separate presentation paints after it
        const bool copiedBase =
            within(base, g_env_light.sun_light_pos, 1.0f) ||
            (s_cameraPainter.seen && within(base, s_cameraPainter.previousSunLightPos, 1.0f));
        if (sunKey && !copiedBase) {
            append_problem(problems,
                "base_light is neither sun_light_pos nor its value before the last camera painter");
        }
        // SetBaseLight reads sun_light_pos, which only a painter with a camera block refreshes
        const double lag = angle_degrees(toBase, toBody);
        if (sunKey && lag > 1.0) {
            return finish(
                fmt::format("key Sun {:.4f} deg from sun_pos; base_light lags it by {:.4f} "
                            "deg until a painter with a camera copies it",
                    bodyDegrees, lag));
        }
        const double baseDegrees = angle_degrees(keyToBody, toBase);
        if (baseDegrees > 1.0) {
            append_problem(problems, fmt::format("{:.4f} deg from base_light", baseDegrees));
        }
        return finish(fmt::format("key {} {:.4f} deg", kind_name(lights.key), baseDegrees));
    }
    // base_light is the game's GX slot 2, rewritten only when a room with LGT data is programmed
    const bool settled = room.count > 0 && last_painter_had_camera();
    const std::string source = base_light_source(base);
    if (settled && source.empty()) {
        append_problem(problems, "base_light matches no displayed LGT0 or env slot");
    }
    const std::string where = source.empty() ? std::string{"an older programmed position"} : source;
    if (lights.key == LightKind::None) {
        if (room.count > 0 && light_switch_on(room.info[0], lights.lightRoom) &&
            !body_black(lights.lightRoom, 0))
        {
            append_problem(problems, "no key although the light room's LGT0 is on and lit");
        }
        return finish(fmt::format("no key; base_light at {}", where));
    }
    if (!keyFirst) {
        append_problem(problems,
            fmt::format("list[0] is not the {} key with priority 255", kind_name(lights.key)));
        return finish("key check");
    }
    if (lights.key != LightKind::KeyRoom || room.count == 0 ||
        !within(get(list.lights[0].position), room.info[0].position, 1e-3f))
    {
        append_problem(problems, "the key is not KeyRoom at the light room's LGT0");
    } else if (lights.rooms[lights.lightRoom][0].state != SlotState::Key) {
        append_problem(problems, fmt::format("room {} slot 0 reports {}", lights.lightRoom,
                                     state_name(lights.rooms[lights.lightRoom][0].state)));
    }
    return finish(
        fmt::format("key KeyRoom room {} LGT0, base_light at {}", lights.lightRoom, where));
}

bool game_slot_empty(SlotTableId table, size_t index) {
    switch (table) {
    case SlotTableId::Point:
        return g_env_light.pointlight[index] == nullptr;
    case SlotTableId::Effect:
        return g_env_light.efplight[index] == nullptr;
    case SlotTableId::EnvSlot:
        return g_env_light.field_0x0c18[index].field_0x26 != 1;
    case SlotTableId::Queued:
        return g_env_light.field_0x0d58[index].field_0x26 == 0;
    case SlotTableId::Dungeon: {
        const int stay = dComIfGp_roomControl_getStayNo();
        return !room_displayed(stay) || static_cast<int>(index) >= room_lights(stay).count;
    }
    }
    return true;
}

bool unused_slot(SlotTableId table, size_t index) noexcept {
    return (table == SlotTableId::EnvSlot && index >= static_cast<size_t>(kEnvSlotsUsed)) ||
           (table == SlotTableId::Dungeon && index >= static_cast<size_t>(kRoomLights));
}

std::string slot_coverage_problems(const SlotReport& report) {
    std::string problems;
    size_t total = 0;
    for (const SlotTable& table : slot_tables(report)) {
        total += table.entries.size();
        for (size_t i = 0; i < table.entries.size(); ++i) {
            const SlotState state = table.entries[i].state;
            const bool unused = unused_slot(table.id, i);
            if (static_cast<size_t>(state) >= kSlotStateCount) {
                append_problem(problems, fmt::format("{}[{}] has no state", table.name, i));
            } else if (unused != (state == SlotState::Unused)) {
                append_problem(
                    problems, fmt::format("{}[{}] {}", table.name, i, state_name(state)));
            } else if (!unused && (state == SlotState::Empty) != game_slot_empty(table.id, i)) {
                append_problem(problems, fmt::format("{}[{}] {} against the game slot", table.name,
                                             i, state_name(state)));
            }
        }
    }
    if (total != kReportSlots) {
        append_problem(problems, fmt::format("{} slots", total));
    }
    return problems;
}

CheckResult check_slot_coverage() {
    const auto scratch = make_scratch();
    build_scene_lights(*scratch);
    std::string problems = slot_coverage_problems(scratch->lights.slots);
    const std::string order = order_problems(scratch->lights.list);
    if (!order.empty()) {
        append_problem(problems, "order: " + order);
    }
    size_t emitted = 0;
    for (const SlotTable& table : slot_tables(scratch->lights.slots)) {
        emitted += std::ranges::count(table.entries, SlotState::Emitted, &SlotEntry::state);
    }
    return verdict(problems, fmt::format("{} slots, {} emitted, {} lights in order", kReportSlots,
                                 emitted, scratch->lights.list.count));
}

struct Snapshot {
    std::vector<u8> bytes;
    const void* source = nullptr;

    void take(const void* from, size_t size) {
        source = from;
        bytes.resize(size);
        std::memcpy(bytes.data(), from, size);
    }

    bool unchanged() const {
        return source == nullptr || std::memcmp(bytes.data(), source, bytes.size()) == 0;
    }
};

using RndState = std::array<s32, 3>;

// cM_rnd2 and its float forms are not in the call count, so the generator states are compared
std::pair<RndState, RndState> rnd_states() {
    std::pair<RndState, RndState> states{};
    cM_getRndState(states.first.data());
    cM_getRnd2State(states.second.data());
    return states;
}

// Everything the builders read from, so a check can prove a build wrote none of it
struct GameSnapshot {
    Snapshot env;
    std::array<Snapshot, kRoomCount> rooms;
    Snapshot player;
    Snapshot camera;
    Snapshot sun;
    Snapshot counter;
    u32 rnd = 0;
    std::pair<RndState, RndState> states{};

    void take() {
        env.take(&g_env_light, sizeof(g_env_light));
        for (int room = 0; room < kRoomCount; ++room) {
            rooms[room].take(dComIfGp_roomControl_getTevStr(room), sizeof(dKy_tevstr_c));
        }
        if (const fopAc_ac_c* actor = dComIfGp_getPlayer(0)) {
            player.take(actor, sizeof(fopAc_ac_c));
        }
        if (const camera_process_class* view = dComIfGp_getCamera(0)) {
            camera.take(view, sizeof(camera_process_class));
        }
        if (g_env_light.mpSunPacket != nullptr) {
            sun.take(g_env_light.mpSunPacket, sizeof(*g_env_light.mpSunPacket));
        }
        counter.take(&g_Counter, sizeof(g_Counter));
        rnd = cM_getRndCallCount();
        states = rnd_states();
    }

    std::string problems() const {
        std::string problems;
        if (!env.unchanged()) {
            append_problem(problems, "g_env_light changed");
        }
        for (int room = 0; room < kRoomCount; ++room) {
            if (!rooms[room].unchanged()) {
                append_problem(problems, fmt::format("room {} tevstr changed", room));
            }
        }
        if (!player.unchanged()) {
            append_problem(problems, "player actor changed");
        }
        if (!camera.unchanged()) {
            append_problem(problems, "camera 0 changed");
        }
        if (!sun.unchanged()) {
            append_problem(problems, "sun packet changed");
        }
        if (!counter.unchanged()) {
            append_problem(problems, "g_Counter changed");
        }
        if (cM_getRndCallCount() != rnd) {
            append_problem(problems, fmt::format("{} cM_rnd calls", cM_getRndCallCount() - rnd));
        }
        if (rnd_states() != states) {
            append_problem(problems, "a random generator state moved");
        }
        return problems;
    }
};

CheckResult check_read_only() {
    auto before = std::make_unique<GameSnapshot>();
    before->take();
    const auto scratch = make_scratch();
    build_scene_lights(*scratch);
    return verdict(before->problems(),
        fmt::format("{} lights built, nothing written", scratch->lights.list.count));
}

CheckResult check_aurora_agrees() {
    if (!hints_active()) {
        return skip("hints are off");
    }
    const ar::HintCounters counters = ar::hint_counters(GX_AURORA_RENDER_LIGHTS);
    // A list sent in the frame still being recorded has not reached the decoder yet
    const u64 sent = s_stats.submits -
                     (s_stats.any && s_stats.lastFrame == aurora::gfx::current_frame() ? 1 : 0);
    std::string problems;
    if (counters.decoded != sent) {
        append_problem(problems,
            fmt::format("aurora decoded {} light lists, dusk sent {}", counters.decoded, sent));
    }
    if (counters.malformed != 0 || counters.late != 0) {
        append_problem(
            problems, fmt::format("malformed {} late {}", counters.malformed, counters.late));
    }
    return verdict(
        problems, fmt::format("decoded {} = sent, bytes {}", counters.decoded, counters.bytes));
}

struct LiveCheck {
    std::string_view name;
    CheckResult (*run)();
};

constexpr std::array<LiveCheck, 5> kLiveChecks{{
    {"lights.sun_matches_game", &check_sun_matches_game},
    {"lights.key_matches_base_light", &check_key_matches_base_light},
    {"lights.slot_coverage", &check_slot_coverage},
    {"lights.read_only", &check_read_only},
    {"lights.aurora_agrees", &check_aurora_agrees},
}};

void run_checks(std::vector<std::string>& out) {
    constexpr std::array<std::string_view, 3> kVerdicts{"PASS", "FAIL", "SKIP"};
    for (const LiveCheck& check : kLiveChecks) {
        const CheckResult result = check.run();
        out.push_back(fmt::format("render.lights check: {} {} {}",
            kVerdicts[static_cast<size_t>(result.verdict)], check.name, result.detail));
    }
    const SceneLights& lights = s_live.lights;
    if (list_available() && lights.slot0) {
        std::string line = fmt::format("render.lights check: frame {} slot0=({:.2f},{:.2f},{:.2f})",
            lights.frame, lights.slot0Pos.x, lights.slot0Pos.y, lights.slot0Pos.z);
        line += lights.flame ? fmt::format(" flame=({:.2f},{:.2f},{:.2f})", lights.flamePos.x,
                                   lights.flamePos.y, lights.flamePos.z) :
                               std::string{" flame=none"};
        const size_t index = find_kind(lights.list, LightKind::PlayerLight);
        if (index != lights.list.count) {
            const Vec pos = get(lights.list.lights[index].position);
            line += fmt::format(" light=({:.2f},{:.2f},{:.2f})", pos.x, pos.y, pos.z);
        }
        out.push_back(std::move(line));
    }
}

void describe_counts(std::vector<std::string>& out) {
    out.push_back(fmt::format(
        "render.lights: submits {}, lights {}, max {}, bytes {}, dropped {}, last frame {}",
        s_stats.submits, s_stats.lights, s_stats.maxCount, s_stats.bytes, s_stats.dropped,
        s_stats.lastFrame));
    const ar::HintCounters counters = ar::hint_counters(GX_AURORA_RENDER_LIGHTS);
    out.push_back(
        fmt::format("render.lights: aurora decoded {} skipped {} malformed {} late {} bytes {}",
            counters.decoded, counters.skipped, counters.malformed, counters.late, counters.bytes));
}

bool parse_count(std::string_view text, u32& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::string test_sky_angle() {
    std::string problems;
    constexpr std::array<std::pair<f32, f32>, 4> kRows{
        {{90.0f, 105.0f}, {180.0f, 180.0f}, {270.0f, 255.0f}, {0.0f, 360.0f}}};
    for (const auto& [daytime, angle] : kRows) {
        if (std::fabs(sky_angle(daytime) - angle) > 1e-3f) {
            append_problem(
                problems, fmt::format("sky_angle({}) = {}", daytime, sky_angle(daytime)));
        }
    }
    // The sun is above the horizon while the y of its offset is positive
    const auto up = [](f32 daytime) { return sky_offset(sky_angle(daytime)).y > 0.0f; };
    if (up(77.1f) || !up(77.2f) || !up(282.8f) || up(282.9f)) {
        append_problem(problems, "horizon crossings outside 77.1-77.2 and 282.8-282.9");
    }
    return problems;
}

std::string test_crossfade() {
    std::string problems;
    constexpr std::array<std::pair<f32, f32>, 9> kRows{
        {{60.0f, 0.0f}, {67.5f, 0.0f}, {75.0f, 0.5f}, {82.5f, 1.0f}, {180.0f, 1.0f}, {277.5f, 1.0f},
            {285.0f, 0.5f}, {292.5f, 0.0f}, {300.0f, 0.0f}}};
    for (const auto& [daytime, weight] : kRows) {
        if (std::fabs(sun_weight(daytime) - weight) > 1e-5f) {
            append_problem(problems, fmt::format("wSun({}) = {}", daytime, sun_weight(daytime)));
        }
    }
    if (sun_is_key(67.5f) || sun_is_key(292.5f) || !sun_is_key(67.6f) || !sun_is_key(292.4f)) {
        append_problem(problems, "key window is not (67.5, 292.5)");
    }
    return problems;
}

std::string test_attn_radius() {
    std::string problems;
    struct Row {
        f32 ref;
        f32 brightness;
        u8 fn;
        f32 expected;
        f32 tolerance;
    };
    constexpr std::array<Row, 9> kRows{{
        {1.0f, kRefBrightness, GX_DA_STEEP, 5036.4f, 0.5f},
        {0.5f, kRefBrightness, GX_DA_STEEP, 2518.2f, 0.5f},
        {1.0f, kRefBrightness, GX_DA_GENTLE, kMaxLightRadius, 0.0f},
        {1.0f, kRefBrightness, GX_DA_MEDIUM, 7122.0f, 2.0f},
        {1.0f, kRefBrightness, GX_DA_OFF, kMaxLightRadius, 0.0f},
        {0.0f, kRefBrightness, GX_DA_STEEP, 0.0f, 0.0f},
        {1e-6f, kRefBrightness, GX_DA_STEEP, 0.0f, 0.0f},
        {1.0f, 1.0f, GX_DA_STEEP, kMaxLightRadius, 0.0f},
        {4.0f, kRefBrightness, GX_DA_STEEP, 20145.6f, 2.0f},
    }};
    for (const Row& row : kRows) {
        const f32 radius = attn_radius(row.ref, row.brightness, row.fn);
        if (std::fabs(radius - row.expected) > row.tolerance) {
            append_problem(problems,
                fmt::format("ref {} b {} fn {} gives {}", row.ref, row.brightness, row.fn, radius));
        }
    }
    if (falloff_code(GX_DA_STEEP) != 2.0f || falloff_code(GX_DA_MEDIUM) != 1.5f ||
        falloff_code(GX_DA_GENTLE) != 1.0f || falloff_code(GX_DA_OFF) != 0.0f)
    {
        append_problem(problems, "falloff codes");
    }
    return problems;
}

std::string test_spot_shape() {
    std::string problems;
    if (spot_from(GX_SP_FLAT, 0.0f, 0.0f, 0.0f).spot || spot_from(GX_SP_FLAT, 95.0f, 0, 0).spot ||
        spot_from(GX_SP_OFF, 45.0f, 0, 0).spot)
    {
        append_problem(problems, "cutoff 0, 95 or SP_OFF is a spot");
    }
    const SpotShape flat = spot_from(GX_SP_FLAT, 45.0f, 0.0f, 0.0f);
    if (!flat.spot || std::fabs(flat.outer - std::cos(kPi / 4.0)) > 1e-5 ||
        std::fabs(flat.inner - (flat.outer + 0.001f)) > 1e-6f)
    {
        append_problem(problems, fmt::format("FLAT 45 gives ({}, {})", flat.inner, flat.outer));
    }
    const SpotShape cos30 = spot_from(GX_SP_COS, 30.0f, 0.0f, 0.0f);
    if (!cos30.spot || cos30.inner != 1.0f || std::fabs(cos30.outer - std::cos(kPi / 6.0)) > 1e-5) {
        append_problem(problems, fmt::format("COS 30 gives ({}, {})", cos30.inner, cos30.outer));
    }
    const auto near = [](const Vec& v, const Vec& e) { return length(sub(v, e)) <= 1e-5f; };
    if (!near(flat.direction, {1.0f, 0.0f, 0.0f})) {
        append_problem(problems, "angles (0, 0) do not point along +x");
    }
    if (!near(spot_from(GX_SP_FLAT, 45.0f, 90.0f, 0.0f).direction, {0.0f, 1.0f, 0.0f})) {
        append_problem(problems, "angles (90, 0) do not point along +y");
    }
    return problems;
}

std::string test_camera_fill_rule() {
    struct Row {
        FillInputs in;
        bool fill;
        std::string_view what;
    };
    const std::array<Row, 12> rows{{
        {{0, false, -1, 0, 0, true, false}, true, "type 0 outdoors"},
        {{0, false, -1, 0, 0, false, false}, false, "id -1 indoors"},
        {{0, false, 3, 500, 200, true, false}, false, "near a point light"},
        {{0, false, 3, 5000, 200, true, false}, false, "id >= 0 but far"},
        {{0, false, 3, 5000, 200, false, true}, true, "D_MN09A"},
        {{0, false, -1, 0, 0, false, true}, true, "D_MN09A indoors"},
        {{1, false, 3, 10, 200, false, false}, true, "type 1 near a light"},
        {{7, false, -1, 0, 0, false, false}, true, "type 7"},
        {{2, true, -1, 0, 0, false, false}, true, "type 2 in twilight"},
        {{2, false, -1, 0, 0, false, false}, false, "type 2 in the light world"},
        {{9, true, 3, 10, 200, false, false}, true, "wolf in twilight"},
        {{0, false, 3, 999.9995f, 0, false, false}, false, "power clamped to 0.001"},
    }};
    std::string problems;
    for (const Row& row : rows) {
        if (camera_fill(row.in) != row.fill) {
            append_problem(problems, row.what);
        }
    }
    return problems;
}

struct SlotZeroStep {
    std::string_view what;
    u8 active;
    u8 alpha;
    f32 ref;
    GXColor color;
    bool flame;
    LightKind kind;
    bool lantern;
    int generation;
};

std::string test_player_light() {
    std::string problems;
    struct Row {
        int index;
        u8 active;
        u8 alpha;
        SlotState state;
        LightKind kind;
    };
    constexpr std::array<Row, 7> kRows{{
        {0, 1, 255, SlotState::Emitted, LightKind::PlayerLight},
        {0, 0, 255, SlotState::Empty, LightKind::None},
        {0, 1, 254, SlotState::Emitted, LightKind::TwilightCamera},
        {0, 1, 253, SlotState::Emitted, LightKind::WaterCamera},
        {3, 1, 255, SlotState::Emitted, LightKind::BossSpot},
        {5, 1, 253, SlotState::Emitted, LightKind::WaterCamera},
        {6, 1, 255, SlotState::Unused, LightKind::None},
    }};
    for (const Row& row : kRows) {
        const EnvSlotClass slot = classify_env_slot(row.index, row.active, row.alpha);
        if (slot.state != row.state || slot.kind != row.kind) {
            append_problem(problems, fmt::format("slot {} alpha {} gives {} {}", row.index,
                                         row.alpha, state_name(slot.state), kind_name(slot.kind)));
        }
    }
    // Lit, fading, sword light, then gone and relit, then taken by a twilight camera light
    constexpr GXColor kLantern{181, 112, 40, 255};
    constexpr GXColor kSword{150, 174, 141, 255};
    constexpr std::array<SlotZeroStep, 9> kSteps{{
        {"lantern", 1, 255, 1.0f, kLantern, true, LightKind::PlayerLight, true, 1},
        {"fade", 1, 255, 0.6f, kLantern, false, LightKind::PlayerLight, false, 1},
        {"sword", 1, 255, 0.4f, kSword, false, LightKind::PlayerLight, false, 1},
        {"empty", 0, 255, 0.4f, kSword, false, LightKind::None, false, 0},
        {"relit", 1, 255, 1.0f, kLantern, true, LightKind::PlayerLight, true, 2},
        {"fading in", 1, 255, 0.0f, kLantern, true, LightKind::None, false, 0},
        {"lit again", 1, 255, 1.0f, kLantern, true, LightKind::PlayerLight, true, 3},
        {"twilight", 1, 254, 3.0f, kSword, false, LightKind::TwilightCamera, false, 1},
        {"lantern back", 1, 255, 1.0f, kLantern, true, LightKind::PlayerLight, true, 4},
    }};
    auto state = std::make_unique<LightState>();
    const StageKey stage{{'F', '_', 'S', 'P', '1', '0', '8'}, 0, 1};
    std::array<BOSS_LIGHT, kEnvSlotCount> slots{};
    std::array<BOSS_LIGHT, kQueuedCount> queued{};
    u32 lanternId = 0;
    for (const SlotZeroStep& step : kSteps) {
        BOSS_LIGHT& b = slots[0];
        b.field_0x26 = step.active;
        b.mColor = step.color;
        b.mColor.a = step.alpha;
        b.mRefDistance = step.ref;
        b.mDistAttenuation = GX_DA_STEEP;
        b.mAngleAttenuation = GX_SP_OFF;
        state->lights.reset();
        state->lights.flame = step.flame;
        state->ids.begin_build(stage);
        BuildContext ctx{state->lights, state->ids};
        add_env_slots(ctx, {slots.data(), queued.data(), 2, 0.0f});
        const LightList& list = state->lights.list;
        const LightKind kind = list.count != 0 ? list.notes[0].kind : LightKind::None;
        if (kind != step.kind || list.count > 1) {
            append_problem(problems,
                fmt::format("{}: {} lights, first {}", step.what, list.count, kind_name(kind)));
            continue;
        }
        if (kind == LightKind::None) {
            continue;
        }
        const AuroraLight& light = list.lights[0];
        const bool lantern = (light.flags & AURORA_LIGHT_LANTERN) != 0;
        if (lantern != step.lantern || id_generation(light.id) != step.generation) {
            append_problem(problems, fmt::format("{}: id 0x{:08X} flags {}", step.what, light.id,
                                         flag_names(light.flags)));
        }
        if (kind == LightKind::PlayerLight) {
            lanternId = step.generation == 1 && lanternId == 0 ? light.id : lanternId;
        }
    }
    if (lanternId != make_light_id(LightKind::PlayerLight, 0, 1)) {
        append_problem(problems, fmt::format("first slot 0 id 0x{:08X}", lanternId));
    }
    return problems;
}

std::string test_ids() {
    std::string problems;
    const StageKey stageA{{'F', '_', 'S', 'P', '1', '0', '3'}, 0, 1};
    const StageKey stageB{{'F', '_', 'S', 'P', '1', '0', '4'}, 0, 2};
    StageKey reloadA = stageA;
    reloadA.epoch = 3;
    const int lightA = 1;
    const int lightB = 2;
    auto run = [&](LightIds& ids, const StageKey& stage, const void* point) {
        ids.begin_build(stage);
        return std::array<u32, 2>{
            ids.assign(LightKind::Sun, 0, nullptr), ids.assign(LightKind::Point, 7, point)};
    };
    const auto gen = [](u32 id) { return static_cast<int>(id_generation(id)); };
    auto one = std::make_unique<LightIds>();
    auto two = std::make_unique<LightIds>();
    const auto a1 = run(*one, stageA, &lightA);
    if (a1 != run(*two, stageA, &lightA) || run(*one, stageA, &lightA) != a1) {
        append_problem(problems, "the same inputs give different ids");
    }
    const auto changed = run(*one, stageA, &lightB);
    if (changed[0] != a1[0] || gen(changed[1]) != gen(a1[1]) + 1) {
        append_problem(problems, "a pointer change does not add 1 to that generation only");
    }
    const auto loaded = run(*one, stageB, &lightB);
    if (gen(loaded[0]) != gen(a1[0]) + 1 || gen(loaded[1]) != gen(changed[1]) + 1) {
        append_problem(problems, "a stage change does not bump every generation");
    }
    run(*one, stageA, &lightB);
    const auto before = run(*one, stageA, &lightB);
    const auto reloaded = run(*one, reloadA, &lightB);
    if (gen(reloaded[0]) != gen(before[0]) + 1 || gen(reloaded[1]) != gen(before[1]) + 1) {
        append_problem(problems, "a reload of the same stage keeps the ids");
    }
    if (next_generation(0xFFFF) != 1 || next_generation(0) != 1 || next_generation(1) != 2) {
        append_problem(problems, "generation 65535 does not wrap to 1");
    }
    if (a1[1] != make_light_id(LightKind::Point, 7, 1)) {
        append_problem(problems, fmt::format("point id 0x{:08X}", a1[1]));
    }
    return problems;
}

std::string test_order_truncation() {
    std::string problems;
    auto list = std::make_unique<LightList>();
    constexpr std::array<std::pair<LightKind, int>, 10> kRuns{{{LightKind::Sun, 1},
        {LightKind::Moon, 1}, {LightKind::CameraFill, 1}, {LightKind::PlayerLight, 1},
        {LightKind::BossSpot, 4}, {LightKind::TwilightCamera, 1}, {LightKind::Point, 100},
        {LightKind::Effect, 3}, {LightKind::ThunderFlash, 1}, {LightKind::Room, 187}}};
    u32 id = 0;
    for (const auto& [kind, count] : kRuns) {
        for (int i = 0; i < count; ++i) {
            AuroraLight light = make_light(AURORA_LIGHT_POINT);
            light.id = ++id;
            LightNote note;
            note.kind = kind;
            note.slot = static_cast<u16>(kind == LightKind::BossSpot ? i + 1 : i);
            if (kind == LightKind::ThunderFlash) {
                note.slot = 3;
            } else if (kind == LightKind::Room) {
                note.neighbour = i >= 6;
                note.room = static_cast<s8>(i < 6 ? 9 : 2 + (i - 6) / 6);
                note.slot = static_cast<u16>(note.room * kRoomLights + i % 6);
            }
            list->add(light, note);
        }
    }
    if (id != 300 || list->count != AURORA_MAX_LIGHTS || list->dropped != 44) {
        append_problem(
            problems, fmt::format("{} lights: kept {} dropped {}", id, list->count, list->dropped));
    }
    for (u32 i = 0; i < list->count; ++i) {
        if (list->lights[i].id != i + 1) {
            append_problem(problems, fmt::format("light {} is id {}", i, list->lights[i].id));
            break;
        }
    }
    if (const std::string order = order_problems(*list); !order.empty()) {
        append_problem(problems, "kind order rejected: " + order);
    }
    std::swap(list->notes[3], list->notes[4]);
    std::swap(list->notes[120], list->notes[121]);
    if (order_problems(*list).empty()) {
        append_problem(problems, "a swapped kind order passes");
    }
    RoomMask displayed{};
    displayed[2] = displayed[5] = displayed[7] = true;
    std::array<s8, kRoomCount> order{};
    if (room_order(5, displayed, order) != 3 || order[0] != 5 || order[1] != 2 || order[2] != 7) {
        append_problem(problems, "neighbour rooms do not follow the light room");
    }
    displayed[5] = false;
    if (room_order(5, displayed, order) != 2 || order[0] != 2 || order[1] != 7) {
        append_problem(problems, "an undisplayed light room is visited");
    }
    return problems;
}

std::string test_linear_colour() {
    std::string problems;
    if (linear_channel(0.0f) != 0.0f || std::fabs(linear_channel(255.0f) - 1.0f) > 1e-6f ||
        std::fabs(linear_channel(128.0f) - 0.2158f) > 1e-4f || linear_channel(-5.0f) != 0.0f)
    {
        append_problem(problems, fmt::format("linear 0/128/255 = {}/{}/{}", linear_channel(0.0f),
                                     linear_channel(128.0f), linear_channel(255.0f)));
    }
    if (!(linear_channel(300.0f) > linear_channel(255.0f)) ||
        !(linear_channel(600.0f) > linear_channel(300.0f)))
    {
        append_problem(problems, "not monotonic above 255");
    }
    return problems;
}

struct GateProbe {
    u32 builds = 0;
    u32 emits = 0;
    u16 count = 0;
    const AuroraLight* lights = nullptr;
    u16 buildCount = 0;
};

GateProbe s_gate;

void probe_build(LightState& state) {
    ++s_gate.builds;
    state.lights.list.count = s_gate.buildCount;
}

void probe_emit(const AuroraLight* lights, u16 count) {
    ++s_gate.emits;
    s_gate.count = count;
    s_gate.lights = lights;
}

std::string test_gate() {
    std::string problems;
    auto state = std::make_unique<LightState>();
    state->lights.list.count = 7;
    s_gate = {};
    const SubmitSeam closed{[]() noexcept { return false; }, &probe_build, &probe_emit};
    if (submit_through(closed, *state) || s_gate.builds != 0 || s_gate.emits != 0 ||
        state->lights.list.count != 7)
    {
        append_problem(problems, "a closed gate still built or sent a list");
    }
    const SubmitSeam open{[]() noexcept { return true; }, &probe_build, &probe_emit};
    for (const u16 count : {u16{0}, u16{3}}) {
        const GateProbe before = s_gate;
        s_gate.buildCount = count;
        if (!submit_through(open, *state) || s_gate.builds != before.builds + 1 ||
            s_gate.emits != before.emits + 1 || s_gate.count != count ||
            s_gate.lights != state->lights.list.lights.data())
        {
            append_problem(
                problems, fmt::format("a painter with {} lights did not send it once", count));
        }
    }
    return problems;
}

void record(SelfTestReport& report, std::string_view name, const CheckResult& result) {
    switch (result.verdict) {
    case Verdict::Pass:
        ++report.passed;
        break;
    case Verdict::Fail:
        report.failed.push_back(fmt::format("{}: {}", name, result.detail));
        break;
    case Verdict::Skip:
        report.skipped.push_back(fmt::format("{}: {}", name, result.detail));
        break;
    }
}

void record_test(SelfTestReport& report, std::string_view name, const std::string& failure) {
    record(report, name, failure.empty() ? CheckResult{} : CheckResult{Verdict::Fail, failure});
}

static_assert(sizeof(AuroraEnvironment) == 236);
static_assert(offsetof(AuroraEnvironment, fogRangeTable) == 212);

// SP6 E7 measured 201-871 u/s per unit of pow by stage; M15-W1b replaces it with a per-stage table
constexpr double kWindWorldSpeed = 420.0;
constexpr double kWindPhasePeriod = 65536.0;
constexpr std::array<double, 3> kFlutterHz{1.7, 3.1, 5.3};
// A longer gap is a stalled or resumed clock, not wind to integrate
constexpr u32 kMaxWindTicks = 30;
constexpr double kTwoPi = 2.0 * kPi;

constexpr std::string_view kEnvUsage = "Usage: render.env [show|check|counts|log <n>]";

// dKy_Outdoor_check reads stage info without a null check, so a stage without it counts as a field
u32 stage_type() {
    stage_stag_info_class* info = dComIfGp_getStage()->getStagInfo();
    return info != nullptr ? dStage_stagInfo_GetSTType(info) : ST_FIELD;
}

// checkNowWolfPowerUp reads through the Link actor, which a loading scene does not have yet
bool senses_on() {
    return dComIfGp_getLinkPlayer() != nullptr && daPy_py_c::checkNowWolfPowerUp() != 0;
}

struct FlagInputs {
    u32 stageType = ST_FIELD;
    bool twilight = false;
    bool underwater = false;
    bool senses = false;
    int rain = 0;
    int snowCount = 0;
    bool sunMoon = true;
};

u32 environment_flags(const FlagInputs& in) noexcept {
    u32 flags = 0;
    if (in.stageType != ST_FIELD) {
        flags |= AURORA_ENV_INTERIOR;
    }
    if (in.twilight) {
        flags |= AURORA_ENV_TWILIGHT;
    }
    if (in.underwater) {
        flags |= AURORA_ENV_UNDERWATER;
    }
    if (in.senses) {
        flags |= AURORA_ENV_SENSES | AURORA_ENV_NO_VOLUMETRIC;
    }
    if (in.rain > 0) {
        flags |= AURORA_ENV_RAIN;
    }
    if (in.snowCount > 0) {
        flags |= AURORA_ENV_SNOW;
    }
    if (!in.sunMoon) {
        flags |= AURORA_ENV_NO_SUN;
    }
    if (in.stageType == ST_ROOM) {
        flags |= AURORA_ENV_ROOM;
    }
    return flags;
}

FlagInputs live_flag_inputs(bool sunMoon) {
    FlagInputs in;
    in.stageType = stage_type();
    in.twilight = dKy_darkworld_check() != 0;
    in.underwater = dKy_camera_water_in_status_check() != 0;
    in.senses = senses_on();
    in.rain = dKy_rain_check();
    in.snowCount = g_env_light.mSnowCount;
    in.sunMoon = sunMoon;
    return in;
}

// The game hands these to GX as u8, so the s16 palette values wrap here the same way
void put_gx_colour(f32 (&dst)[3], std::array<u8, 3>& gx, const GXColorS10& colour) noexcept {
    gx = {static_cast<u8>(colour.r), static_cast<u8>(colour.g), static_cast<u8>(colour.b)};
    for (size_t i = 0; i < gx.size(); ++i) {
        dst[i] = gx[i] / 255.0f;
    }
}

struct ViewRange {
    f32 nearZ = kNoViewNear;
    f32 farZ = kNoViewFar;
};

// The same checks as GxFogSet_Sub, so the hint carries the planes GXSetFog was given
ViewRange fog_view_range() {
    ViewRange range;
    const view_class* view = dComIfGd_getView();
    if (view != nullptr && view->near_ >= 0.0f && view->far_ >= 0.0f && view->near_ < view->far_) {
        range = {view->near_, view->far_};
    }
    return range;
}

constexpr u32 pack_range_adj(u8 enable, u16 center) noexcept {
    return (enable != 0 ? 1u : 0u) | static_cast<u32>(center) << 16;
}

void put_fog(AuroraEnvironment& env) {
    env.fogType = GX_FOG_PERSP_LIN;
    env.fogStart = g_env_light.mFogNear;
    env.fogEnd = g_env_light.mFogFar;
    const ViewRange view = fog_view_range();
    env.viewNear = view.nearZ;
    env.viewFar = view.farZ;
    env.fogRangeAdj = pack_range_adj(g_env_light.mFogAdjEnable, g_env_light.mFogAdjCenter);
    std::copy(std::begin(g_env_light.mXFogTbl.r), std::end(g_env_light.mXFogTbl.r),
        std::begin(env.fogRangeTable));
}

// Negative values ask for the engine default; phaseG spans [-1, 1], so its sentinel is -2
void apply_env_defaults(AuroraEnvironment& env) noexcept {
    env.hazeAtFogEnd = -1.0f;
    env.hazeDensity = -1.0f;
    env.heightFogDensity = 0.0f;
    env.heightFogBase = 0.0f;
    env.heightFogFalloff = 0.0f;
    std::fill(std::begin(env.mediaAlbedo), std::end(env.mediaAlbedo), -1.0f);
    std::fill(std::begin(env.mediaEmissive), std::end(env.mediaEmissive), 0.0f);
    env.phaseG = -2.0f;
    env.phaseG2 = -2.0f;
    env.phaseBlend = -1.0f;
    env.noiseScale = -1.0f;
    env.noiseAmount = -1.0f;
    env.relightStrength = -1.0f;
    env.relightAmbient = -1.0f;
    env.sunGlow = -1.0f;
    env.volumeFar = -1.0f;
    std::fill(std::begin(env.fogRangeReserved), std::end(env.fogRangeReserved), u16{0});
}

struct ShaftInputs {
    bool sunInitialized = false;
    bool packet = false;
    f32 daytime = 0.0f;
    int rain = 0;
    bool flareStage = true;
    u8 colpatWeather = 0;
    int snowCount = 0;
    bool room = false;
    bool sunMoon = false;
    f32 sunAlpha = 0.0f;
};

// dKyr_sun_move's conditions for fading the sun in and letting its flare draw
f32 shaft_gate(const ShaftInputs& in) noexcept {
    if (!in.sunInitialized || !in.packet) {
        return 0.0f;
    }
    const bool on = in.daytime > 77.5f && in.daytime < 285.0f && in.rain < 20 && in.flareStage &&
                    in.colpatWeather == 0 && in.snowCount <= 10 && !in.room && in.sunMoon;
    return on ? std::clamp(in.sunAlpha, 0.0f, 1.0f) : 0.0f;
}

ShaftInputs live_shaft_inputs(bool sunMoon) {
    ShaftInputs in;
    in.sunInitialized = g_env_light.mSunInitialized;
    in.packet = g_env_light.mpSunPacket != nullptr;
    in.daytime = g_env_light.daytime;
    in.rain = dKy_rain_check();
    in.flareStage = !stage_is("F_SP200") && !stage_is("D_MN09B");
    in.colpatWeather = g_env_light.mColpatWeather;
    in.snowCount = g_env_light.mSnowCount;
    in.room = stage_type() == ST_ROOM;
    in.sunMoon = sunMoon;
    in.sunAlpha = in.packet ? g_env_light.mpSunPacket->mSunAlpha : 0.0f;
    return in;
}

struct WindVector {
    Vec dir{};
    f32 strength = 0.0f;
};

// A NaN would stay in the phase accumulators until the next load, so it counts as calm
WindVector wind_from(const Vec& vec, f32 pow) noexcept {
    WindVector wind;
    const bool finite =
        std::isfinite(vec.x) && std::isfinite(vec.y) && std::isfinite(vec.z) && std::isfinite(pow);
    if (finite && normalize(vec, wind.dir, 1e-6f)) {
        wind.strength = pow;
    }
    return wind;
}

struct WindState {
    std::array<double, 2> phase{};
    std::array<double, 3> flutter{};
    std::array<double, 2> presentedPhase{};
    u32 lastCounter = 0;
    StageKey stage{};
    u64 presentationEpoch = 0;
    bool valid = false;
};

struct WindInputs {
    WindVector wind;
    u32 counter = 0;
    StageKey stage{};
    u64 presentationEpoch = 0;
    f32 step = 1.0f;
};

struct WindStep {
    u32 ticks = 0;
    bool reset = false;
};

double wrap_phase(double value, double period) noexcept {
    const double wrapped = value - period * std::floor(value / period);
    return wrapped >= period ? wrapped - period : wrapped;
}

// Phases count sim ticks, so they hold while the game clock stops and keep running in menus
WindStep advance_wind(WindState& state, const WindInputs& in, AuroraEnvironment& env) noexcept {
    WindStep step;
    step.ticks = in.counter - state.lastCounter;
    const bool newLoad = !state.valid || !(state.stage == in.stage);
    step.reset =
        newLoad || state.presentationEpoch != in.presentationEpoch || step.ticks > kMaxWindTicks;
    if (step.reset) {
        step.ticks = 0;
    }
    if (newLoad) {
        state.phase = {};
        state.flutter = {};
    }
    const double dt = dusk::game_clock::kSimPeriod;
    const double behind = (1.0 - in.step) * dt;
    const double speed = static_cast<double>(in.wind.strength) * kWindWorldSpeed;
    const std::array<double, 2> velocity{in.wind.dir.x * speed, in.wind.dir.z * speed};
    for (size_t i = 0; i < velocity.size(); ++i) {
        state.phase[i] += velocity[i] * dt * step.ticks;
        const double presented = state.phase[i] - behind * velocity[i];
        const double current = wrap_phase(presented, kWindPhasePeriod);
        const double previous =
            step.reset ? current : current - (presented - state.presentedPhase[i]);
        env.windPhase[i] = static_cast<f32>(current);
        env.windPhasePrev[i] = static_cast<f32>(previous);
        state.presentedPhase[i] = presented;
    }
    const double sweep = 0.25 + 0.75 * static_cast<double>(in.wind.strength);
    for (size_t k = 0; k < kFlutterHz.size(); ++k) {
        const double rate = kTwoPi * kFlutterHz[k] * sweep;
        state.flutter[k] += rate * dt * step.ticks;
        env.flutterPhase[k] =
            static_cast<f32>(wrap_phase(state.flutter[k] - behind * rate, kTwoPi));
    }
    state.lastCounter = in.counter;
    state.stage = in.stage;
    state.presentationEpoch = in.presentationEpoch;
    state.valid = true;
    return step;
}

f32 presentation_step() {
    return dusk::interp::is_enabled() ? dusk::interp::get_interpolation_step() : 1.0f;
}

struct EnvReport {
    std::array<u8, 3> ambientGx{};
    std::array<u8, 3> fogGx{};
    bool skyVisible = false;
    StageKey stage{};
    int room = -1;
    f32 daytime = 0.0f;
    u32 frame = 0;
    u32 counter = 0;
    WindStep wind;
};

struct EnvState {
    AuroraEnvironment env{};
    WindState wind;
    EnvReport report;
};

void build_environment(EnvState& state, LoadTracker& loads, bool sunMoon) {
    AuroraEnvironment& env = state.env;
    EnvReport& report = state.report;
    env = {};
    report = {};
    apply_env_defaults(env);
    report.stage = current_stage_key(loads);
    report.frame = aurora::gfx::current_frame();
    report.room = dComIfGp_roomControl_getStayNo();
    report.daytime = g_env_light.daytime;
    report.counter = g_Counter.mCounter0;

    report.skyVisible = sky_visible();
    env.skyVisibility = report.skyVisible ? 1.0f : 0.0f;
    put_gx_colour(env.ambientColor, report.ambientGx, g_env_light.bg_amb_col[0]);
    put_gx_colour(env.fogColor, report.fogGx, g_env_light.fog_col);
    const GB_WIND_INFLUENCE& global = g_env_light.global_wind_influence;
    const WindVector wind = wind_from(global.vec, global.pow);
    put(env.windDir, wind.dir);
    env.windStrength = wind.strength;
    env.timeOfDay = g_env_light.daytime / 15.0f;
    env.flags = environment_flags(live_flag_inputs(sunMoon));
    env.unitsPerMeter = kUnitsPerMeter;
    const WindInputs windIn{wind, report.counter, report.stage,
        dusk::game_clock::g_frameTiming.presentationEpoch, presentation_step()};
    report.wind = advance_wind(state.wind, windIn, env);
    put_fog(env);
    env.shadowSun = g_env_light.field_0x1238;
    env.shadowLocal = g_env_light.field_0x123c;
    const EF_THUNDER& thunder = g_env_light.mThunderEff;
    env.shadowThunderAdd = thunder.mState != 0 && thunder.mState < 10 ? thunder.mFlashTimer : 0.0f;
    env.grassDensityScale =
        stage_is("F_SP127") && g_env_light.fishing_hole_season == 4 ? 0.0f : 1.0f;
    env.sunShaftGate = shaft_gate(live_shaft_inputs(sunMoon));
}

enum class ProbeField : u8 {
    None,
    FogType,
    FogStart,
    FogEnd,
    ViewNear,
    ViewFar,
    FogColor,
    RangeEnable,
    RangeCenter,
    RangeTable,
};

std::string_view probe_field_name(ProbeField field) noexcept {
    constexpr std::array<std::string_view, 10> kNames{"none", "fogType", "fogStart", "fogEnd",
        "viewNear", "viewFar", "fogColor", "fogRangeAdj.enable", "fogRangeAdj.center",
        "fogRangeTable"};
    const auto index = static_cast<size_t>(field);
    return index < kNames.size() ? kNames[index] : "?";
}

// The window is one painter: armed after the hint is sent, closed after FRAME_END
struct FogProbe {
    bool pending = false;
    bool armed = false;
    AuroraEnvironment checked{};
    u32 frame = 0;
    u32 fog = 0;
    u32 range = 0;
    ProbeField field = ProbeField::None;
    int index = -1;
    double hint = 0.0;
    double draw = 0.0;
};

struct ProbeResult {
    bool pass = false;
    std::string line;
};

std::string probe_request(FogProbe& probe, bool hintsOn) {
    if (!hintsOn) {
        return "render.env check: SKIP (hints off)";
    }
    probe.pending = true;
    return "render.env check: armed for the next scene frame";
}

void probe_arm(FogProbe& probe, const AuroraEnvironment& env, u32 frame) noexcept {
    probe.armed = true;
    probe.checked = env;
    probe.frame = frame;
    probe.fog = 0;
    probe.range = 0;
    probe.field = ProbeField::None;
    probe.index = -1;
}

void probe_mismatch(
    FogProbe& probe, ProbeField field, double hint, double draw, int index = -1) noexcept {
    if (probe.field == ProbeField::None) {
        probe.field = field;
        probe.index = index;
        probe.hint = hint;
        probe.draw = draw;
    }
}

bool same_bits(f32 a, f32 b) noexcept {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

void probe_fog(FogProbe& probe, GXFogType type, f32 startZ, f32 endZ, f32 nearZ, f32 farZ,
    GXColor color) noexcept {
    if (!probe.armed) {
        return;
    }
    ++probe.fog;
    const AuroraEnvironment& env = probe.checked;
    if (static_cast<u32>(type) != env.fogType) {
        probe_mismatch(probe, ProbeField::FogType, env.fogType, type);
    }
    const std::array<std::tuple<ProbeField, f32, f32>, 4> planes{{
        {ProbeField::FogStart, env.fogStart, startZ},
        {ProbeField::FogEnd, env.fogEnd, endZ},
        {ProbeField::ViewNear, env.viewNear, nearZ},
        {ProbeField::ViewFar, env.viewFar, farZ},
    }};
    for (const auto& [field, hint, draw] : planes) {
        if (!same_bits(hint, draw)) {
            probe_mismatch(probe, field, hint, draw);
        }
    }
    const std::array<u8, 3> drawn{color.r, color.g, color.b};
    for (size_t i = 0; i < drawn.size(); ++i) {
        const auto sent = static_cast<u8>(std::lround(env.fogColor[i] * 255.0f));
        if (drawn[i] != sent) {
            probe_mismatch(probe, ProbeField::FogColor, sent, drawn[i], static_cast<int>(i));
        }
    }
}

void probe_range(FogProbe& probe, u8 enable, u16 center, const GXFogAdjTable& table) noexcept {
    if (!probe.armed) {
        return;
    }
    ++probe.range;
    const u32 packed = probe.checked.fogRangeAdj;
    if ((enable != 0) != ((packed & 1) != 0)) {
        probe_mismatch(probe, ProbeField::RangeEnable, packed & 1, enable);
    }
    if (center != packed >> 16) {
        probe_mismatch(probe, ProbeField::RangeCenter, packed >> 16, center);
    }
    for (size_t i = 0; i < std::size(table.r); ++i) {
        if (table.r[i] != probe.checked.fogRangeTable[i]) {
            probe_mismatch(probe, ProbeField::RangeTable, probe.checked.fogRangeTable[i],
                table.r[i], static_cast<int>(i));
        }
    }
}

ProbeResult probe_close(FogProbe& probe) {
    ProbeResult result;
    probe.armed = false;
    probe.pending = false;
    if (probe.fog == 0) {
        result.line = fmt::format(
            "render.env check: FAIL no environment-fog program (frame {})", probe.frame);
    } else if (probe.field != ProbeField::None) {
        const std::string index = probe.index >= 0 ? fmt::format("[{}]", probe.index) : "";
        result.line = fmt::format("render.env check: FAIL {}{} hint={:.9g} draw={:.9g} (frame {})",
            probe_field_name(probe.field), index, probe.hint, probe.draw, probe.frame);
    } else {
        result.pass = true;
        result.line = fmt::format("render.env check: PASS fog={} range={} (frame {})", probe.fog,
            probe.range, probe.frame);
    }
    return result;
}

struct EnvStats {
    u64 submits = 0;
    bool any = false;
    u32 lastFrame = 0;
};

EnvState s_env;
EnvStats s_envStats;
u32 s_envLogRemaining = 0;
FogProbe s_probe;
ProbeResult s_probeResult;
bool s_probeRan = false;

std::unique_ptr<EnvState> build_scratch_environment(bool sunMoon) {
    auto state = std::make_unique<EnvState>();
    state->wind = s_env.wind;
    LoadTracker loads = s_live.loads;
    build_environment(*state, loads, sunMoon);
    return state;
}

std::string triple_text(const f32 (&v)[3]) {
    return fmt::format("({:g},{:g},{:g})", v[0], v[1], v[2]);
}

std::string gx_text(const std::array<u8, 3>& gx) {
    return fmt::format("({},{},{})", gx[0], gx[1], gx[2]);
}

std::string table_text(std::span<const u16> values) {
    return fmt::format("{{{}}}", fmt::join(values, ","));
}

void describe_environment(std::vector<std::string>& out) {
    if (!has_main_camera() || !s_envStats.any) {
        out.emplace_back("render.env: hints off or no main camera");
        return;
    }
    const AuroraEnvironment& e = s_env.env;
    const EnvReport& r = s_env.report;
    const auto line = [&out](std::string text) { out.push_back("render.env: " + std::move(text)); };
    const auto scalar = [&line](std::string_view name, f32 value) {
        line(fmt::format("{}={:g}", name, value));
    };
    scalar("skyVisibility", e.skyVisibility);
    line(fmt::format("ambientColor={} gx={}", triple_text(e.ambientColor), gx_text(r.ambientGx)));
    line(fmt::format("fogColor={} gx={}", triple_text(e.fogColor), gx_text(r.fogGx)));
    line(fmt::format("windDir={}", triple_text(e.windDir)));
    scalar("windStrength", e.windStrength);
    scalar("exposureBias", e.exposureBias);
    scalar("timeOfDay", e.timeOfDay);
    line(fmt::format("flags=0x{:X} {}", e.flags, env_flag_names(e.flags)));
    scalar("unitsPerMeter", e.unitsPerMeter);
    line(fmt::format("windPhase=({:.3f},{:.3f})", e.windPhase[0], e.windPhase[1]));
    line(fmt::format("windPhasePrev=({:.3f},{:.3f})", e.windPhasePrev[0], e.windPhasePrev[1]));
    line(fmt::format("flutterPhase=({:.4f},{:.4f},{:.4f})", e.flutterPhase[0], e.flutterPhase[1],
        e.flutterPhase[2]));
    line(fmt::format("fogType={}", e.fogType));
    scalar("fogStart", e.fogStart);
    scalar("fogEnd", e.fogEnd);
    scalar("viewNear", e.viewNear);
    scalar("viewFar", e.viewFar);
    line(fmt::format("fogRangeAdj=0x{:08X} enable={} center={}", e.fogRangeAdj, e.fogRangeAdj & 1,
        e.fogRangeAdj >> 16));
    scalar("shadowSun", e.shadowSun);
    scalar("shadowLocal", e.shadowLocal);
    scalar("shadowThunderAdd", e.shadowThunderAdd);
    scalar("grassDensityScale", e.grassDensityScale);
    scalar("sunShaftGate", e.sunShaftGate);
    scalar("hazeAtFogEnd", e.hazeAtFogEnd);
    scalar("hazeDensity", e.hazeDensity);
    scalar("heightFogDensity", e.heightFogDensity);
    scalar("heightFogBase", e.heightFogBase);
    scalar("heightFogFalloff", e.heightFogFalloff);
    line(fmt::format("mediaAlbedo={}", triple_text(e.mediaAlbedo)));
    line(fmt::format("mediaEmissive={}", triple_text(e.mediaEmissive)));
    scalar("phaseG", e.phaseG);
    scalar("phaseG2", e.phaseG2);
    scalar("phaseBlend", e.phaseBlend);
    scalar("noiseScale", e.noiseScale);
    scalar("noiseAmount", e.noiseAmount);
    scalar("relightStrength", e.relightStrength);
    scalar("relightAmbient", e.relightAmbient);
    scalar("sunGlow", e.sunGlow);
    scalar("volumeFar", e.volumeFar);
    line(fmt::format("fogRangeTable={}", table_text(e.fogRangeTable)));
    line(fmt::format("fogRangeReserved={}", table_text(e.fogRangeReserved)));
    line(fmt::format("skyVisible={} shaftGate={:g} stage={} room={} daytime={:.2f}",
        r.skyVisible ? 1 : 0, e.sunShaftGate, stage_text(r.stage), r.room, r.daytime));
    line(fmt::format("frame={} counter={} windTicks={} windReset={}", r.frame, r.counter,
        r.wind.ticks, r.wind.reset ? 1 : 0));
}

void describe_env_counts(std::vector<std::string>& out) {
    out.push_back(fmt::format("render.env: submits {}, bytes {}, last frame {}", s_envStats.submits,
        s_envStats.submits * kEnvironmentHintBytes, s_envStats.lastFrame));
    const ar::HintCounters counters = ar::hint_counters(GX_AURORA_RENDER_ENVIRONMENT);
    out.push_back(
        fmt::format("render.env: aurora decoded {} skipped {} malformed {} late {} bytes {}",
            counters.decoded, counters.skipped, counters.malformed, counters.late, counters.bytes));
}

CheckResult check_fog_fields() {
    if (dComIfGp_getCamera(0) == nullptr) {
        return skip("no camera 0");
    }
    const auto state = build_scratch_environment(sun_moon_lit());
    const AuroraEnvironment& env = state->env;
    std::string problems;
    const auto plane = [&problems](std::string_view name, f32 sent, f32 source) {
        if (!same_bits(sent, source)) {
            append_problem(problems, fmt::format("{} {:.9g} against {:.9g}", name, sent, source));
        }
    };
    // Restated from GxFogSet_Sub rather than shared with the builder
    f32 nearZ = 1.0f;
    f32 farZ = 160000.0f;
    if (const view_class* view = dComIfGd_getView(); view != nullptr) {
        if (view->near_ >= 0.0f && view->far_ >= 0.0f && view->near_ < view->far_) {
            nearZ = view->near_;
            farZ = view->far_;
        }
    }
    if (env.fogType != GX_FOG_PERSP_LIN) {
        append_problem(problems, fmt::format("fogType {}", env.fogType));
    }
    plane("fogStart", env.fogStart, g_env_light.mFogNear);
    plane("fogEnd", env.fogEnd, g_env_light.mFogFar);
    plane("viewNear", env.viewNear, nearZ);
    plane("viewFar", env.viewFar, farZ);
    const GXColorS10& fog = g_env_light.fog_col;
    const std::array<u8, 3> fogGx{
        static_cast<u8>(fog.r), static_cast<u8>(fog.g), static_cast<u8>(fog.b)};
    for (size_t i = 0; i < fogGx.size(); ++i) {
        if (!same_bits(env.fogColor[i], fogGx[i] / 255.0f)) {
            append_problem(problems,
                fmt::format("fogColor[{}] {:.9g} against {}", i, env.fogColor[i], fogGx[i]));
        }
    }
    const u32 packed = (g_env_light.mFogAdjEnable != 0 ? 1u : 0u) |
                       static_cast<u32>(g_env_light.mFogAdjCenter) << 16;
    if (env.fogRangeAdj != packed) {
        append_problem(problems,
            fmt::format("fogRangeAdj 0x{:08X} against 0x{:08X}", env.fogRangeAdj, packed));
    }
    if (!std::equal(std::begin(env.fogRangeTable), std::end(env.fogRangeTable),
            std::begin(g_env_light.mXFogTbl.r)))
    {
        append_problem(problems, "fogRangeTable differs from mXFogTbl");
    }
    if (env.fogRangeReserved[0] != 0 || env.fogRangeReserved[1] != 0) {
        append_problem(problems, "fogRangeReserved is not 0");
    }
    return verdict(problems,
        fmt::format("fog {:g}..{:g} view {:g}..{:g} colour {} table {}", env.fogStart, env.fogEnd,
            env.viewNear, env.viewFar, gx_text(fogGx), table_text(env.fogRangeTable)));
}

// A failing pure leg still fails the case when its live leg cannot run
CheckResult without_live_leg(const std::string& problems) {
    if (!problems.empty()) {
        return {Verdict::Fail, problems};
    }
    return skip("the pure rows pass; the live leg needs camera 0");
}

std::string flag_rule_problems() {
    struct Row {
        std::string_view what;
        FlagInputs in;
        u32 flags;
    };
    const auto with = [](auto change) {
        FlagInputs in;
        change(in);
        return in;
    };
    const auto all = [](FlagInputs& in) { in = {ST_ROOM, true, true, true, 3, 12, false}; };
    const std::array<Row, 15> rows{{
        {"field, sun and moon", {}, 0},
        {"dungeon", with([](FlagInputs& in) { in.stageType = ST_DUNGEON; }), AURORA_ENV_INTERIOR},
        {"room", with([](FlagInputs& in) { in.stageType = ST_ROOM; }),
            AURORA_ENV_INTERIOR | AURORA_ENV_ROOM},
        {"boss room", with([](FlagInputs& in) { in.stageType = ST_BOSS_ROOM; }),
            AURORA_ENV_INTERIOR},
        {"castle town", with([](FlagInputs& in) { in.stageType = ST_CASTLE_TOWN; }),
            AURORA_ENV_INTERIOR},
        {"cloud sea", with([](FlagInputs& in) { in.stageType = ST_CLOUD_SEA; }),
            AURORA_ENV_INTERIOR},
        {"twilight", with([](FlagInputs& in) { in.twilight = true; }), AURORA_ENV_TWILIGHT},
        {"underwater", with([](FlagInputs& in) { in.underwater = true; }), AURORA_ENV_UNDERWATER},
        {"senses", with([](FlagInputs& in) { in.senses = true; }),
            AURORA_ENV_SENSES | AURORA_ENV_NO_VOLUMETRIC},
        {"rain 1", with([](FlagInputs& in) { in.rain = 1; }), AURORA_ENV_RAIN},
        {"rain -1", with([](FlagInputs& in) { in.rain = -1; }), 0},
        {"snow 1", with([](FlagInputs& in) { in.snowCount = 1; }), AURORA_ENV_SNOW},
        {"snow 0", with([](FlagInputs& in) { in.snowCount = 0; }), 0},
        {"SunMoon fails", with([](FlagInputs& in) { in.sunMoon = false; }), AURORA_ENV_NO_SUN},
        {"everything", with(all), 0x1FF},
    }};
    std::string problems;
    for (const Row& row : rows) {
        const u32 flags = environment_flags(row.in);
        if (flags != row.flags) {
            append_problem(problems, fmt::format("{} gives {}", row.what, env_flag_names(flags)));
        }
    }
    return problems;
}

// The light list shows its sun decision in what it emitted: a sun or moon key, or KEY_ROOM
std::string no_sun_problem(const SceneLights& lights, u32 flags, std::string_view what) {
    const bool noSun = (flags & AURORA_ENV_NO_SUN) != 0;
    bool body = false;
    for (size_t i = 0; i < lights.list.count; ++i) {
        const LightKind kind = lights.list.notes[i].kind;
        body = body || kind == LightKind::Sun || kind == LightKind::Moon;
    }
    const bool bodyKey = lights.key == LightKind::Sun || lights.key == LightKind::Moon;
    if ((bodyKey || body) && noSun) {
        return fmt::format("{}: NO_SUN set with the {} key", what, kind_name(lights.key));
    }
    if (lights.key == LightKind::KeyRoom && !noSun) {
        return fmt::format("{}: NO_SUN clear with the KeyRoom key", what);
    }
    return {};
}

CheckResult check_env_flags() {
    std::string problems = flag_rule_problems();
    if (dComIfGp_getCamera(0) == nullptr) {
        return without_live_leg(problems);
    }
    const auto state = build_scratch_environment(sun_moon_lit());
    const u32 flags = state->env.flags;
    stage_stag_info_class* info = dComIfGp_getStage()->getStagInfo();
    const bool senses = dComIfGp_getLinkPlayer() != nullptr && daPy_py_c::checkNowWolfPowerUp();
    const std::array<std::pair<u32, bool>, 9> kRows{{
        {AURORA_ENV_INTERIOR, info != nullptr && dKy_Indoor_check() == TRUE},
        {AURORA_ENV_TWILIGHT, dKy_darkworld_check() != 0},
        {AURORA_ENV_UNDERWATER, dKy_camera_water_in_status_check() != 0},
        {AURORA_ENV_SENSES, senses},
        {AURORA_ENV_RAIN, dKy_rain_check() > 0},
        {AURORA_ENV_SNOW, g_env_light.mSnowCount > 0},
        {AURORA_ENV_NO_SUN, dKy_SunMoon_Light_Check() != TRUE},
        {AURORA_ENV_NO_VOLUMETRIC, senses},
        {AURORA_ENV_ROOM, info != nullptr && dStage_stagInfo_GetSTType(info) == ST_ROOM},
    }};
    u32 known = 0;
    for (const auto& [bit, expected] : kRows) {
        known |= bit;
        if (((flags & bit) != 0) != expected) {
            append_problem(problems,
                fmt::format("{} is {}", env_flag_names(bit), (flags & bit) != 0 ? "set" : "clear"));
        }
    }
    if ((flags & ~known) != 0) {
        append_problem(problems, fmt::format("unknown bits 0x{:X}", flags & ~known));
    }
    // A scratch list built on its own, then the pair this painter actually sent
    const auto lights = make_scratch();
    build_scene_lights(*lights);
    if (std::string mismatch = no_sun_problem(lights->lights, flags, "scratch"); !mismatch.empty())
    {
        append_problem(problems, mismatch);
    }
    std::string sent = "no pair sent in the last painter";
    if (s_envStats.any && s_stats.any && s_env.report.frame == s_live.lights.frame) {
        const std::string what = fmt::format("frame {}", s_env.report.frame);
        if (std::string mismatch = no_sun_problem(s_live.lights, s_env.env.flags, what);
            !mismatch.empty())
        {
            append_problem(problems, mismatch);
        }
        sent = fmt::format("frame {} sent key {} with NO_SUN {}", s_env.report.frame,
            kind_name(s_live.lights.key),
            (s_env.env.flags & AURORA_ENV_NO_SUN) != 0 ? "set" : "clear");
    }
    return verdict(problems, fmt::format("flags {}; scratch key {}; {}", env_flag_names(flags),
                                 kind_name(lights->lights.key), sent));
}

CheckResult check_sky_visible() {
    const bool predicate = sky_visible();
    // wether_move_vrkumo's own conditions for drawing the clouds of the sky box
    bool direct = false;
    const bool skyBox = dComIfGp_checkStatus(1) != 0 && !g_env_light.hide_vrbox;
    BOOL vrboxSwitch = FALSE;
    roomRead_class* room = dComIfGp_getStageRoom();
    const int stay = dComIfGp_roomControl_getStayNo();
    if (room != nullptr && stay >= 0 && room->num > stay) {
        vrboxSwitch = dStage_roomRead_dt_c_GetVrboxswitch(*room->m_entries[stay]);
    }
    if (skyBox) {
        direct = stage_is("F_SP200") || vrboxSwitch != 0;
    }
    std::string problems;
    if (predicate != direct) {
        append_problem(problems, fmt::format("sky_visible() {} against {}", predicate, direct));
    }
    const auto state = build_scratch_environment(sun_moon_lit());
    if (state->env.skyVisibility != (predicate ? 1.0f : 0.0f)) {
        append_problem(problems, fmt::format("skyVisibility {:g}", state->env.skyVisibility));
    }
    return verdict(
        problems, fmt::format("skyVisible {} (sky box {}, hide_vrbox {}, vrbox switch {}, stay {})",
                      predicate ? 1 : 0, dComIfGp_checkStatus(1) != 0 ? 1 : 0,
                      g_env_light.hide_vrbox ? 1 : 0, vrboxSwitch != 0 ? 1 : 0, stay));
}

// The sent values are f32, and just below the 65536 u wrap one f32 step is 2^-8 u
constexpr double kWindStepTolerance = 1e-2;
constexpr double kFlutterTolerance = 1e-5;

double flutter_rate(size_t k, f32 strength) noexcept {
    return kTwoPi * kFlutterHz[k] * (0.25 + 0.75 * static_cast<double>(strength)) *
           dusk::game_clock::kSimPeriod;
}

std::string test_wind_phase() {
    std::string problems;
    const StageKey stage{{'F', '_', 'S', 'P', '1', '2', '1'}, 6, 1};
    const WindVector wind{{1.0f, 0.0f, 0.0f}, static_cast<f32>(600.0 / kWindWorldSpeed)};
    const double tick =
        static_cast<double>(wind.strength) * kWindWorldSpeed * dusk::game_clock::kSimPeriod;
    WindState state;
    AuroraEnvironment env{};
    WindInputs in{wind, 1000, stage, 1, 1.0f};
    if (!advance_wind(state, in, env).reset || env.windPhase[0] != env.windPhasePrev[0]) {
        append_problem(problems, "the first build is not a reset");
    }
    const auto step = [&env](size_t i) {
        return static_cast<double>(env.windPhase[i]) - static_cast<double>(env.windPhasePrev[i]);
    };
    const auto in_turn = [](f32 phase) { return phase >= 0.0f && phase < kTwoPi; };
    state.phase[0] = kWindPhasePeriod - 900.0;
    state.presentedPhase[0] = state.phase[0];
    bool wrapped = false;
    bool flutterWrapped = false;
    for (u32 t = 1; t <= 90; ++t) {
        const f32 before = env.windPhase[0];
        std::array<f32, 3> flutterBefore{};
        std::copy(std::begin(env.flutterPhase), std::end(env.flutterPhase), flutterBefore.begin());
        in.counter = 1000 + t;
        const WindStep result = advance_wind(state, in, env);
        wrapped = wrapped || env.windPhase[0] < before;
        if (result.reset || result.ticks != 1 || std::fabs(step(0) - tick) > kWindStepTolerance ||
            step(1) != 0.0 || env.windPhase[0] < 0.0f || !(env.windPhase[0] < kWindPhasePeriod))
        {
            append_problem(problems, fmt::format("tick {}: phase {} prev {}", t, env.windPhase[0],
                                         env.windPhasePrev[0]));
            break;
        }
        std::string flutterProblem;
        for (size_t k = 0; k < kFlutterHz.size() && flutterProblem.empty(); ++k) {
            double moved = static_cast<double>(env.flutterPhase[k]) - flutterBefore[k];
            if (moved < 0.0) {
                moved += kTwoPi;
                flutterWrapped = true;
            }
            if (!in_turn(env.flutterPhase[k]) ||
                std::fabs(moved - flutter_rate(k, wind.strength)) > kFlutterTolerance)
            {
                flutterProblem = fmt::format(
                    "tick {}: flutter[{}] {} moved {}", t, k, env.flutterPhase[k], moved);
            }
        }
        if (!flutterProblem.empty()) {
            append_problem(problems, flutterProblem);
            break;
        }
    }
    if (!wrapped || !flutterWrapped) {
        append_problem(problems, "90 ticks never crossed a wrap");
    }
    // A second presentation at the same tick and step shows the same phases
    std::array<f32, 5> held{env.windPhase[0], env.windPhase[1], env.flutterPhase[0],
        env.flutterPhase[1], env.flutterPhase[2]};
    advance_wind(state, in, env);
    if (held != std::array<f32, 5>{env.windPhase[0], env.windPhase[1], env.flutterPhase[0],
                    env.flutterPhase[1], env.flutterPhase[2]})
    {
        append_problem(problems, "the phases move when no tick passes");
    }
    const auto expect_reset = [&](std::string_view what) {
        const WindStep result = advance_wind(state, in, env);
        if (!result.reset || env.windPhase[0] != env.windPhasePrev[0] ||
            env.windPhase[1] != env.windPhasePrev[1])
        {
            append_problem(problems, fmt::format("{} does not give prev == cur", what));
        }
    };
    in.stage.epoch = 2;
    in.counter += 1;
    expect_reset("a stage-key change");
    if (env.windPhase[0] != 0.0f || env.flutterPhase[0] != 0.0f || env.flutterPhase[1] != 0.0f ||
        env.flutterPhase[2] != 0.0f)
    {
        append_problem(problems, "a new load does not start the phases at 0");
    }
    in.presentationEpoch = 2;
    in.counter += 1;
    expect_reset("a presentation epoch change");
    in.counter += kMaxWindTicks + 1;
    expect_reset("a gap of more than 30 ticks");
    // Two presentations of one tick add up to that tick
    in.counter += 1;
    in.step = 0.5f;
    advance_wind(state, in, env);
    const double first = step(0);
    in.step = 1.0f;
    advance_wind(state, in, env);
    if (std::fabs(first - tick / 2.0) > kWindStepTolerance ||
        std::fabs(first + step(0) - tick) > kWindStepTolerance)
    {
        append_problem(problems, fmt::format("half steps {} and {}", first, step(0)));
    }
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    const std::array<std::pair<std::string_view, WindVector>, 5> calmRows{{
        {"a zero vector", wind_from({0.0f, 0.0f, 0.0f}, 0.7f)},
        {"a vector under 1e-6", wind_from({0.0f, 0.0f, 1e-7f}, 0.7f)},
        {"a NaN strength", wind_from({0.0f, 0.0f, 1.0f}, nan)},
        {"a NaN vector", wind_from({nan, 0.0f, 1.0f}, 0.7f)},
        {"an infinite vector", wind_from({inf, 0.0f, 0.0f}, 0.7f)},
    }};
    for (const auto& [what, calm] : calmRows) {
        if (calm.strength != 0.0f || length(calm.dir) != 0.0f) {
            append_problem(problems, fmt::format("{} does not give a calm wind", what));
        }
    }
    const WindVector breeze = wind_from({0.0f, 0.0f, 2.0f}, 0.3f);
    if (breeze.strength != 0.3f || !within(breeze.dir, {0.0f, 0.0f, 1.0f}, 1e-6f)) {
        append_problem(problems, "a breeze is not normalized");
    }
    in.wind = calmRows[0].second;
    in.counter += 1;
    advance_wind(state, in, env);
    const f32 still = env.windPhase[0];
    in.counter += 5;
    advance_wind(state, in, env);
    if (env.windPhase[0] != still || step(0) != 0.0) {
        append_problem(problems, "the phase moves without wind");
    }
    return problems;
}

std::string shaft_rule_problems() {
    ShaftInputs lit;
    lit.sunInitialized = true;
    lit.packet = true;
    lit.daytime = 150.0f;
    lit.sunMoon = true;
    lit.sunAlpha = 0.75f;
    struct Row {
        std::string_view what;
        ShaftInputs in;
        f32 gate;
    };
    const auto with = [&lit](auto change) {
        ShaftInputs in = lit;
        change(in);
        return in;
    };
    const std::array<Row, 17> rows{{
        {"midday", lit, 0.75f},
        {"no sun yet", with([](ShaftInputs& in) { in.sunInitialized = false; }), 0.0f},
        {"no sun packet", with([](ShaftInputs& in) { in.packet = false; }), 0.0f},
        {"daytime 77.5", with([](ShaftInputs& in) { in.daytime = 77.5f; }), 0.0f},
        {"daytime 77.6", with([](ShaftInputs& in) { in.daytime = 77.6f; }), 0.75f},
        {"daytime 285", with([](ShaftInputs& in) { in.daytime = 285.0f; }), 0.0f},
        {"daytime 284.9", with([](ShaftInputs& in) { in.daytime = 284.9f; }), 0.75f},
        {"rain 20", with([](ShaftInputs& in) { in.rain = 20; }), 0.0f},
        {"rain 19", with([](ShaftInputs& in) { in.rain = 19; }), 0.75f},
        {"F_SP200 or D_MN09B", with([](ShaftInputs& in) { in.flareStage = false; }), 0.0f},
        {"weather pattern", with([](ShaftInputs& in) { in.colpatWeather = 1; }), 0.0f},
        {"snow 11", with([](ShaftInputs& in) { in.snowCount = 11; }), 0.0f},
        {"snow 10", with([](ShaftInputs& in) { in.snowCount = 10; }), 0.75f},
        {"room stage", with([](ShaftInputs& in) { in.room = true; }), 0.0f},
        {"SunMoon fails", with([](ShaftInputs& in) { in.sunMoon = false; }), 0.0f},
        {"alpha 1.5", with([](ShaftInputs& in) { in.sunAlpha = 1.5f; }), 1.0f},
        {"alpha -0.2", with([](ShaftInputs& in) { in.sunAlpha = -0.2f; }), 0.0f},
    }};
    std::string problems;
    for (const Row& row : rows) {
        if (shaft_gate(row.in) != row.gate) {
            append_problem(problems, fmt::format("{} gives {}", row.what, shaft_gate(row.in)));
        }
    }
    return problems;
}

CheckResult check_shaft_gate() {
    std::string problems = shaft_rule_problems();
    if (dComIfGp_getCamera(0) == nullptr) {
        return without_live_leg(problems);
    }
    // dKyr_sun_move's conditions restated from game state, not read through live_shaft_inputs
    const dKankyo_sun_Packet* sun = g_env_light.mpSunPacket;
    stage_stag_info_class* info = dComIfGp_getStage()->getStagInfo();
    const char* name = dComIfGp_getStartStageName();
    const bool room = info != nullptr && dStage_stagInfo_GetSTType(info) == ST_ROOM;
    const bool flareStage = strcmp(name, "F_SP200") != 0 && strcmp(name, "D_MN09B") != 0;
    const bool fadesIn = g_env_light.daytime > 77.5f && g_env_light.daytime < 285.0f &&
                         dKy_rain_check() < 20 && flareStage;
    const bool flare = g_env_light.mColpatWeather == 0 && g_env_light.mSnowCount <= 10 && !room;
    const bool sunMoon = dKy_SunMoon_Light_Check() == TRUE;
    const bool on = g_env_light.mSunInitialized && sun != nullptr && fadesIn && flare && sunMoon;
    const f32 expected = on ? std::clamp(sun->mSunAlpha, 0.0f, 1.0f) : 0.0f;
    const ShaftInputs in = live_shaft_inputs(sun_moon_lit());
    const std::array<std::pair<std::string_view, bool>, 10> kSame{{
        {"sunInitialized", in.sunInitialized == g_env_light.mSunInitialized},
        {"packet", in.packet == (sun != nullptr)},
        {"daytime", same_bits(in.daytime, g_env_light.daytime)},
        {"rain", in.rain == dKy_rain_check()},
        {"flareStage", in.flareStage == flareStage},
        {"colpatWeather", in.colpatWeather == g_env_light.mColpatWeather},
        {"snowCount", in.snowCount == g_env_light.mSnowCount},
        {"room", in.room == room},
        {"sunMoon", in.sunMoon == sunMoon},
        {"sunAlpha", sun == nullptr ? in.sunAlpha == 0.0f : same_bits(in.sunAlpha, sun->mSunAlpha)},
    }};
    for (const auto& [field, same] : kSame) {
        if (!same) {
            append_problem(problems, fmt::format("input {} differs from the game", field));
        }
    }
    const auto state = build_scratch_environment(sunMoon);
    if (shaft_gate(in) != expected || state->env.sunShaftGate != expected) {
        append_problem(problems, fmt::format("gate {:g} and sent {:g} against {:g}", shaft_gate(in),
                                     state->env.sunShaftGate, expected));
    }
    return verdict(
        problems, fmt::format("gate {:g} (fades in {}, flare {}, SunMoon {}, alpha {:g})", expected,
                      fadesIn ? 1 : 0, flare ? 1 : 0, sunMoon ? 1 : 0,
                      sun != nullptr ? sun->mSunAlpha : 0.0f));
}

std::string default_problems(const AuroraEnvironment& env, bool exposure) {
    struct Row {
        std::string_view name;
        f32 value;
        f32 expected;
    };
    const std::array<Row, 21> rows{{
        {"hazeAtFogEnd", env.hazeAtFogEnd, -1.0f},
        {"hazeDensity", env.hazeDensity, -1.0f},
        {"heightFogDensity", env.heightFogDensity, 0.0f},
        {"heightFogBase", env.heightFogBase, 0.0f},
        {"heightFogFalloff", env.heightFogFalloff, 0.0f},
        {"mediaAlbedo.r", env.mediaAlbedo[0], -1.0f},
        {"mediaAlbedo.g", env.mediaAlbedo[1], -1.0f},
        {"mediaAlbedo.b", env.mediaAlbedo[2], -1.0f},
        {"mediaEmissive.r", env.mediaEmissive[0], 0.0f},
        {"mediaEmissive.g", env.mediaEmissive[1], 0.0f},
        {"mediaEmissive.b", env.mediaEmissive[2], 0.0f},
        {"phaseG", env.phaseG, -2.0f},
        {"phaseG2", env.phaseG2, -2.0f},
        {"phaseBlend", env.phaseBlend, -1.0f},
        {"noiseScale", env.noiseScale, -1.0f},
        {"noiseAmount", env.noiseAmount, -1.0f},
        {"relightStrength", env.relightStrength, -1.0f},
        {"relightAmbient", env.relightAmbient, -1.0f},
        {"sunGlow", env.sunGlow, -1.0f},
        {"volumeFar", env.volumeFar, -1.0f},
        {"exposureBias", env.exposureBias, 0.0f},
    }};
    // exposureBias is last: apply_env_defaults leaves it to the zeroed struct
    std::string problems;
    for (const Row& row : std::span(rows).first(exposure ? rows.size() : rows.size() - 1)) {
        if (!same_bits(row.value, row.expected)) {
            append_problem(problems, fmt::format("{} = {:g}", row.name, row.value));
        }
    }
    if (env.fogRangeReserved[0] != 0 || env.fogRangeReserved[1] != 0) {
        append_problem(problems, "fogRangeReserved is not 0");
    }
    return problems;
}

CheckResult check_env_defaults() {
    // Filled first, so a row expecting 0 proves apply_env_defaults wrote it
    AuroraEnvironment filled;
    std::memset(&filled, 0x7F, sizeof filled);
    apply_env_defaults(filled);
    std::string problems = default_problems(filled, false);
    if (pack_range_adj(1, 0x140) != 0x01400001u || pack_range_adj(0, 0xFFFF) != 0xFFFF0000u ||
        pack_range_adj(2, 0) != 1u)
    {
        append_problem(problems, "fogRangeAdj packing");
    }
    if (dComIfGp_getCamera(0) == nullptr) {
        return without_live_leg(problems);
    }
    const auto state = build_scratch_environment(sun_moon_lit());
    const AuroraEnvironment& env = state->env;
    if (const std::string built = default_problems(env, true); !built.empty()) {
        append_problem(problems, "built: " + built);
    }
    if (env.unitsPerMeter != kUnitsPerMeter) {
        append_problem(problems, fmt::format("unitsPerMeter {:g}", env.unitsPerMeter));
    }
    if (!same_bits(env.timeOfDay, g_env_light.daytime / 15.0f)) {
        append_problem(problems,
            fmt::format("timeOfDay {:g} for daytime {:g}", env.timeOfDay, g_env_light.daytime));
    }
    return verdict(problems, fmt::format("sentinels set, unitsPerMeter {:g}, timeOfDay {:g}",
                                 env.unitsPerMeter, env.timeOfDay));
}

std::string test_fog_probe_window() {
    std::string problems;
    AuroraEnvironment env{};
    env.fogType = GX_FOG_PERSP_LIN;
    env.fogStart = 500.0f;
    env.fogEnd = 26000.0f;
    env.viewNear = 5.0f;
    env.viewFar = 320000.0f;
    const GXColor colour{200, 210, 220, 0};
    env.fogColor[0] = colour.r / 255.0f;
    env.fogColor[1] = colour.g / 255.0f;
    env.fogColor[2] = colour.b / 255.0f;
    env.fogRangeAdj = pack_range_adj(1, 0x140);
    const GXFogAdjTable table{{260, 260, 270, 280, 290, 300, 310, 320, 330, 340}};
    std::copy(std::begin(table.r), std::end(table.r), std::begin(env.fogRangeTable));
    const auto matching = [&](FogProbe& probe) {
        probe_fog(probe, GX_FOG_PERSP_LIN, 500.0f, 26000.0f, 5.0f, 320000.0f, colour);
    };

    FogProbe probe;
    if (probe_request(probe, false) != "render.env check: SKIP (hints off)" || probe.pending) {
        append_problem(problems, "hints off does not give the SKIP reply");
    }
    if (probe_request(probe, true) != "render.env check: armed for the next scene frame" ||
        !probe.pending)
    {
        append_problem(problems, "a request does not leave a check pending");
    }
    probe_fog(probe, GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, colour);
    probe_arm(probe, env, 7);
    matching(probe);
    matching(probe);
    probe_range(probe, 1, 0x140, table);
    probe_range(probe, 1, 0x140, table);
    const ProbeResult pass = probe_close(probe);
    if (!pass.pass || pass.line != "render.env check: PASS fog=2 range=2 (frame 7)" ||
        probe.pending || probe.armed)
    {
        append_problem(problems, fmt::format("two matching calls give '{}'", pass.line));
    }
    probe_fog(probe, GX_FOG_NONE, 1.0f, 2.0f, 3.0f, 4.0f, colour);
    probe_range(probe, 0, 0, table);
    if (probe.fog != 2 || probe.range != 2 || probe.field != ProbeField::None) {
        append_problem(problems, "a call after the close was counted");
    }

    GXFogAdjTable bad = table;
    bad.r[3] = 999;
    probe_arm(probe, env, 8);
    matching(probe);
    probe_range(probe, 1, 0x140, bad);
    const ProbeResult tableFail = probe_close(probe);
    if (tableFail.pass || !tableFail.line.starts_with("render.env check: FAIL fogRangeTable[3]")) {
        append_problem(problems, fmt::format("a mismatching table gives '{}'", tableFail.line));
    }

    probe_arm(probe, env, 9);
    probe_fog(
        probe, GX_FOG_PERSP_LIN, std::nextafter(500.0f, 1e9f), 26000.0f, 5.0f, 320000.0f, colour);
    const ProbeResult startFail = probe_close(probe);
    if (startFail.pass || !startFail.line.starts_with("render.env check: FAIL fogStart")) {
        append_problem(problems, fmt::format("a start one ulp off gives '{}'", startFail.line));
    }

    probe_arm(probe, env, 10);
    probe_range(probe, 1, 0x140, table);
    const ProbeResult none = probe_close(probe);
    if (none.pass || none.line != "render.env check: FAIL no environment-fog program (frame 10)") {
        append_problem(problems, fmt::format("no fog call gives '{}'", none.line));
    }
    return problems;
}

CheckResult check_fog_probe() {
    if (!s_probeRan) {
        return skip(s_probe.pending ? "a render.env check is still pending" :
                                      "no render.env check has run");
    }
    return {s_probeResult.pass ? Verdict::Pass : Verdict::Fail, s_probeResult.line};
}

CheckResult check_env_read_only() {
    auto before = std::make_unique<GameSnapshot>();
    before->take();
    const auto lights = make_scratch();
    build_scene_lights(*lights);
    const auto env = build_scratch_environment(lights->lights.sunMoon);
    const bool sky = sky_visible();
    const ViewRange view = fog_view_range();
    const GXColorS10& fog = g_env_light.fog_col;
    FogProbe probe;
    probe_arm(probe, env->env, env->report.frame);
    probe_fog(probe, GX_FOG_PERSP_LIN, g_env_light.mFogNear, g_env_light.mFogFar, view.nearZ,
        view.farZ, {static_cast<u8>(fog.r), static_cast<u8>(fog.g), static_cast<u8>(fog.b), 0});
    probe_range(probe, g_env_light.mFogAdjEnable, g_env_light.mFogAdjCenter, g_env_light.mXFogTbl);
    const ProbeResult result = probe_close(probe);
    return verdict(before->problems(),
        fmt::format("{} lights and the environment built (sky {}, gate {:g}, probe {}), "
                    "nothing written",
            lights->lights.list.count, sky ? 1 : 0, env->env.sunShaftGate,
            result.pass ? "PASS" : "FAIL"));
}

CheckResult check_env_aurora_agrees() {
    if (!hints_active()) {
        return skip("hints are off");
    }
    if (!s_envStats.any) {
        return skip("no environment sent yet");
    }
    const ar::HintCounters counters = ar::hint_counters(GX_AURORA_RENDER_ENVIRONMENT);
    // An environment sent in the frame still being recorded has not reached the decoder yet
    const bool inFlight = s_envStats.any && s_envStats.lastFrame == aurora::gfx::current_frame();
    const u64 sent = s_envStats.submits - (inFlight ? 1 : 0);
    std::string problems;
    if (counters.decoded != sent) {
        append_problem(problems,
            fmt::format("aurora decoded {} environments, dusk sent {}", counters.decoded, sent));
    }
    if (counters.malformed != 0 || counters.late != 0) {
        append_problem(
            problems, fmt::format("malformed {} late {}", counters.malformed, counters.late));
    }
    if (counters.skipped == 0 && counters.bytes != counters.decoded * kEnvironmentHintBytes) {
        append_problem(
            problems, fmt::format("bytes {} for {} hints", counters.bytes, counters.decoded));
    }
    return verdict(
        problems, fmt::format("decoded {} = sent, bytes {}", counters.decoded, counters.bytes));
}

}  // namespace

void submit_lights() {
    ZoneScopedN("dusk::render::submit_lights");
    note_camera_painter();
    if (!submit_through(kLiveSeam, s_live)) {
        return;
    }
    const SceneLights& lights = s_live.lights;
    const u16 count = lights.list.count;
    ++s_stats.submits;
    s_stats.lights += count;
    s_stats.bytes += kLightsFixedBytes + kLightBytes * count;
    s_stats.dropped += lights.list.dropped;
    s_stats.maxCount = std::max(s_stats.maxCount, count);
    s_stats.any = true;
    s_stats.lastFrame = lights.frame;
    if (lights.list.dropped != 0 && !(s_warnedDropped && s_droppedStage == lights.stage)) {
        s_warnedDropped = true;
        s_droppedStage = lights.stage;
        LightLog.warn("render.lights: {} lights past {} dropped in stage {}", lights.list.dropped,
            AURORA_MAX_LIGHTS, stage_text(lights.stage));
    }
    if (s_logRemaining != 0) {
        --s_logRemaining;
        LightLog.info("render.lights: frame={} lights={} bytes={}", lights.frame, count,
            kLightsFixedBytes + kLightBytes * count);
    }
}

bool lights_command(std::span<const std::string> args, std::vector<std::string>& out) {
    const std::string_view verb = args.empty() ? std::string_view{"list"} : args[0];
    u32 count = 0;
    if (args.size() <= 1 && verb == "list") {
        describe_list(out);
    } else if (args.size() == 1 && verb == "slots") {
        describe_slots(out);
    } else if (args.size() == 1 && verb == "check") {
        run_checks(out);
    } else if (args.size() == 1 && verb == "counts") {
        describe_counts(out);
    } else if (args.size() == 2 && verb == "log" && parse_count(args[1], count)) {
        s_logRemaining = count;
        out.push_back(fmt::format("render.lights: logging the next {} submits", count));
    } else {
        out.emplace_back(kUsage);
        return false;
    }
    return true;
}

void lights_self_test(SelfTestReport& report) {
    record_test(report, "lights.sky_angle", test_sky_angle());
    record(report, "lights.sun_matches_game", check_sun_matches_game());
    record(report, "lights.key_matches_base_light", check_key_matches_base_light());
    record_test(report, "lights.crossfade", test_crossfade());
    record_test(report, "lights.attn_radius", test_attn_radius());
    record_test(report, "lights.spot_shape", test_spot_shape());
    record_test(report, "lights.camera_fill_rule", test_camera_fill_rule());
    record_test(report, "lights.player_light", test_player_light());
    record_test(report, "lights.ids", test_ids());
    record_test(report, "lights.order_truncation", test_order_truncation());
    record_test(report, "lights.linear_colour", test_linear_colour());
    record_test(report, "lights.gate", test_gate());
    record(report, "lights.slot_coverage", check_slot_coverage());
    record(report, "lights.read_only", check_read_only());
    record(report, "lights.aurora_agrees", check_aurora_agrees());
}

void submit_environment() {
    ZoneScopedN("dusk::render::submit_environment");
    if (!has_main_camera()) {
        return;
    }
    // NO_SUN takes the sun decision of the light list this painter sent
    const bool listed = s_stats.any && s_live.lights.frame == aurora::gfx::current_frame();
    build_environment(s_env, s_live.loads, listed ? s_live.lights.sunMoon : sun_moon_lit());
    GXAuroraSetEnvironment(&s_env.env);
    ++s_envStats.submits;
    s_envStats.any = true;
    s_envStats.lastFrame = s_env.report.frame;
    if (s_probe.pending) {
        probe_arm(s_probe, s_env.env, s_env.report.frame);
    }
    if (s_envLogRemaining != 0) {
        --s_envLogRemaining;
        LightLog.info("render.env: frame={} bytes={}", s_env.report.frame, kEnvironmentHintBytes);
    }
}

void fog_probe_env(
    GXFogType type, f32 startZ, f32 endZ, f32 nearZ, f32 farZ, GXColor color) noexcept {
    if (s_probe.armed) {
        probe_fog(s_probe, type, startZ, endZ, nearZ, farZ, color);
    }
}

void fog_probe_range(u8 enable, u16 center, const GXFogAdjTable& table) noexcept {
    if (s_probe.armed) {
        probe_range(s_probe, enable, center, table);
    }
}

void fog_probe_close() noexcept {
    if (!s_probe.armed) {
        return;
    }
    s_probeResult = probe_close(s_probe);
    s_probeRan = true;
    LightLog.info("{}", s_probeResult.line);
}

bool sky_visible() {
    if (dComIfGp_checkStatus(1) == 0 || g_env_light.hide_vrbox) {
        return false;
    }
    if (stage_is("F_SP200")) {
        return true;
    }
    roomRead_class* room = dComIfGp_getStageRoom();
    const int stay = dComIfGp_roomControl_getStayNo();
    return room != nullptr && stay >= 0 && room->num > stay &&
           dStage_roomRead_dt_c_GetVrboxswitch(*room->m_entries[stay]) != 0;
}

bool environment_command(std::span<const std::string> args, std::vector<std::string>& out) {
    const std::string_view verb = args.empty() ? std::string_view{"show"} : args[0];
    u32 count = 0;
    if (args.size() <= 1 && verb == "show") {
        describe_environment(out);
    } else if (args.size() == 1 && verb == "check") {
        out.push_back(probe_request(s_probe, ar::hints_enabled()));
    } else if (args.size() == 1 && verb == "counts") {
        describe_env_counts(out);
    } else if (args.size() == 2 && verb == "log" && parse_count(args[1], count)) {
        s_envLogRemaining = count;
        out.push_back(fmt::format("render.env: logging the next {} submits", count));
    } else {
        out.emplace_back(kEnvUsage);
        return false;
    }
    return true;
}

void environment_self_test(SelfTestReport& report) {
    record(report, "env.fog_fields", check_fog_fields());
    record(report, "env.flags", check_env_flags());
    record(report, "env.sky_visible", check_sky_visible());
    record_test(report, "env.wind_phase", test_wind_phase());
    record(report, "env.shaft_gate", check_shaft_gate());
    record(report, "env.defaults", check_env_defaults());
    record_test(report, "env.fog_probe_window", test_fog_probe_window());
    record(report, "env.fog_probe", check_fog_probe());
    record(report, "env.read_only", check_env_read_only());
    record(report, "env.aurora_agrees", check_env_aurora_agrees());
}

}  // namespace dusk::render
