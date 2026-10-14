#include "dusk/input_replay.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/mod_loader.hpp"
#include "dusk/settings.h"
#include "dusk/touch_camera.h"
#include "dusk/ui/ui.hpp"
#include "dusk/version.hpp"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_graphic.h"

#include <borealis/io.hpp>
#include <borealis/version.h>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace dusk::input_replay {
namespace {

static_assert(std::endian::native == std::endian::little);

constexpr char kMagic[8] = {'D', 'L', 'I', 'N', 'P', 'U', 'T', '\0'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kPrefixSize = 16;
constexpr uint32_t kTickRecordSize = 144;
constexpr uint32_t kAnchorSize = 56;
constexpr uint32_t kSaveSize = 0xA94;
constexpr uint32_t kSimHz = 30;
constexpr OSTime kTicksPerSimTick = OS_TIMER_CLOCK / kSimHz;
constexpr uint32_t kPreAnchorTickLimit = 9000;
constexpr uint32_t kPinnedCheckInterval = 30;
constexpr uint32_t kHashLogFlushRows = 300;
constexpr auto kDvdBarrierTimeout = std::chrono::seconds(10);
constexpr auto kWaveBarrierTimeout = std::chrono::seconds(2);
constexpr auto kGateWatchdog = std::chrono::seconds(5);

constexpr uint32_t make_tag(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
           uint32_t(uint8_t(d)) << 24;
}
constexpr uint32_t kTagSave = make_tag('S', 'A', 'V', 'E');
constexpr uint32_t kTagAnchor = make_tag('A', 'N', 'C', 'H');
constexpr uint32_t kTagCommand = make_tag('C', 'M', 'D', ' ');
constexpr uint32_t kTagTick = make_tag('T', 'I', 'C', 'K');
constexpr uint32_t kTagEnd = make_tag('E', 'N', 'D', ' ');

enum TickFlag : uint32_t {
    TickResetSwitch = 1u << 0,
    TickGyroReset = 1u << 1,
    TickLoadBarrierTimeout = 1u << 2,
    TickDvdGateWatchdog = 1u << 3,
};

enum ExitCode : int {
    ExitOk = 0,
    ExitDivergence = 3,
    ExitReplayError = 4,
};

struct TickStateV1 {
    uint32_t tick;
    int32_t rnd[3];
    uint32_t rndCalls;
    uint32_t counter0;
    float timeOfDay;
    char stage[8];
    int32_t room;
    float pos[3];
    int32_t angleY;
    float speedF;
    float eye[3];
    float center[3];
    uint32_t playerPresent;
};
static_assert(sizeof(TickStateV1) == 88);

struct AnchorSnapshot {
    int32_t rnd[3];
    int32_t rnd2[3];
    uint32_t counter0;
    int32_t counter1;
    uint32_t timer;
    uint64_t totalTicks;
    uint64_t anchorHash;
};

struct TickRecord {
    uint32_t tick;
    uint32_t flags;
    TickInput input;
    uint64_t stateHash;
    int32_t rnd[3];
    uint32_t rndCalls;
    uint32_t presRndCalls;
};

struct CommandEvent {
    uint32_t tick;
    std::string line;
};

struct Session {
    Mode mode = Mode::Idle;
    Options options;
    std::string file;
    bool exitOnReplayEnd = false;

    std::ofstream out;
    uint32_t commandsRecorded = 0;
    std::vector<std::pair<std::string, std::string>> pinned;
    bool pinnedChangeWarned = false;

    nlohmann::json header;
    std::vector<TickRecord> ticks;
    std::vector<CommandEvent> commands;
    size_t nextCommand = 0;
    std::optional<AnchorSnapshot> anchorRecord;
    bool hasSave = false;
    std::vector<uint8_t> save;
    CommandState replayCommands;

    uint8_t saveSlot = 0;
    StageRequest stage{};

    bool anchored = false;
    uint32_t tick = 0;
    uint32_t preAnchorTicks = 0;
    TickInput current{};
    uint32_t currentFlags = 0;
    uint32_t rndAtTickStart = 0;
    uint32_t rndAtLastTickEnd = 0;
    uint32_t presRnd = 0;
    uint32_t presRndTotal = 0;
    uint32_t divergentTicks = 0;
    std::optional<uint32_t> firstDivergence;
    uint32_t barrierTimeouts = 0;
    uint64_t lastHash = 0;
    TickStateV1 lastState{};
    bool hasLastState = false;
    int exitCode = ExitOk;

    std::ofstream hashLog;
    uint32_t hashLogRows = 0;
};

Session s;

// dLib timers read this instead of the wall clock so minigame timers replay exactly.
bool s_virtualTime = false;
uint64_t s_totalTicks = 0;
OSTime s_virtualNow = 0;

std::atomic_bool s_gateEnabled{false};
std::atomic_bool s_inTick{false};
std::atomic_bool s_gateWatchdogFired{false};
std::atomic<int32_t> s_dvdPending{0};
std::atomic<int32_t> s_wavePending{0};
std::mutex s_gateMutex;
std::condition_variable s_gateCv;
std::condition_variable s_pendingCv;
bool s_gateOpen = false;

bool is_session_mode() {
    return s.mode == Mode::Recording || s.mode == Mode::Replaying;
}

bool holds_neutral_input() {
    return s.mode == Mode::Finished && s.exitOnReplayEnd;
}

// Boot runs on this in both modes, so live input before the start point never reaches the game.
TickInput neutral_input() {
    TickInput input{};
    for (int port = 1; port < PAD_CHANMAX; ++port) {
        input.pad[port].err = PAD_ERR_NO_CONTROLLER;
    }
    input.gyroReset = true;
    return input;
}

void set_gate_enabled(bool enabled) {
    {
        std::lock_guard lock(s_gateMutex);
        s_gateEnabled.store(enabled, std::memory_order_release);
        s_gateOpen = false;
    }
    s_gateCv.notify_all();
}

std::string utc_now() {
    return fmt::format(FMT_STRING("{:%Y-%m-%dT%H:%M:%SZ}"), fmt::gmtime(std::time(nullptr)));
}

template <typename T>
std::string setting_string(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return fmt::format(FMT_STRING("{}"), static_cast<int64_t>(value));
    } else if constexpr (std::is_floating_point_v<T>) {
        return fmt::format(FMT_STRING("{:.9g}"), value);
    } else {
        return fmt::format(FMT_STRING("{}"), value);
    }
}

using PinnedList = std::vector<std::pair<std::string, std::string>>;

template <typename T>
void pin(PinnedList& out, const config::ConfigVar<T>& var) {
    out.emplace_back(var.getName(), setting_string(var.getValue()));
}

PinnedList pinned_settings() {
    const auto& g = getSettings().game;
    PinnedList out;
    pin(out, g.language);
    pin(out, g.enableQuickTransform);
    pin(out, g.hideTvSettingsScreen);
    pin(out, g.biggerWallets);
    pin(out, g.noReturnRupees);
    pin(out, g.disableRupeeCutscenes);
    pin(out, g.fastTransitions);
    pin(out, g.noSwordRecoil);
    pin(out, g.damageMultiplier);
    pin(out, g.noHeartDrops);
    pin(out, g.instantDeath);
    pin(out, g.fastClimbing);
    pin(out, g.noMissClimbing);
    pin(out, g.fastTears);
    pin(out, g.no2ndFishForCat);
    pin(out, g.buttonFishing);
    pin(out, g.instantSaves);
    pin(out, g.instantText);
    pin(out, g.holdToMash);
    pin(out, g.sunsSong);
    pin(out, g.enhancedMapMenus);
    pin(out, g.aimingReticle);
    pin(out, g.noHitStun);
    pin(out, g.enableMirrorMode);
    pin(out, g.menuScalingMode);
    pin(out, g.midnasLamentNonStop);

    pin(out, g.enableGyroAim);
    pin(out, g.enableGyroRollgoal);
    pin(out, g.gyroSensitivityRollgoal);
    pin(out, g.enableMouseCamera);
    pin(out, g.enableMouseAim);
    pin(out, g.freeCamera);
    pin(out, g.enableTouchControls);
    pin(out, g.touchTargeting);
    pin(out, g.invertCameraXAxis);
    pin(out, g.invertCameraYAxis);
    pin(out, g.invertFirstPersonXAxis);
    pin(out, g.invertFirstPersonYAxis);
    pin(out, g.invertAirSwimX);
    pin(out, g.invertAirSwimY);
    pin(out, g.freeCameraXSensitivity);
    pin(out, g.freeCameraYSensitivity);
    pin(out, g.touchCameraXSensitivity);
    pin(out, g.touchCameraYSensitivity);
    pin(out, g.swapDirectSelect);

    pin(out, g.infiniteHearts);
    pin(out, g.infiniteArrows);
    pin(out, g.infiniteSeeds);
    pin(out, g.infiniteBombs);
    pin(out, g.infiniteOil);
    pin(out, g.infiniteOxygen);
    pin(out, g.infiniteRupees);
    pin(out, g.enableIndefiniteItemDrops);
    pin(out, g.moonJump);
    pin(out, g.superClawshot);
    pin(out, g.alwaysGreatspin);
    pin(out, g.enableFastIronBoots);
    pin(out, g.canTransformAnywhere);
    pin(out, g.fastRoll);
    pin(out, g.fastSpinner);
    pin(out, g.armorRupeeDrain);
    pin(out, g.invincibleEnemies);
    pin(out, g.easyQuickSpin);
    pin(out, g.infiniteEponaStamina);

    pin(out, g.restoreWiiGlitches);
    pin(out, g.speedrunMode);
    pin(out, g.removeQuestMapMarkers);
    pin(out, g.enableMoveLinkCombo);
    pin(out, g.enableTeleportCombo);

    const auto& bindings = getSettings().actionBindings;
    for (const auto* group : {&bindings.firstPersonCamera, &bindings.callMidna,
             &bindings.openMapScreen, &bindings.toggleMinimap})
    {
        for (const auto& var : *group) {
            pin(out, var);
        }
    }
    return out;
}

class ByteWriter {
public:
    void u8(uint8_t v) { mData.push_back(v); }
    void u16(uint16_t v) { append(&v, sizeof(v)); }
    void u32(uint32_t v) { append(&v, sizeof(v)); }
    void s32(int32_t v) { append(&v, sizeof(v)); }
    void u64(uint64_t v) { append(&v, sizeof(v)); }
    void f32(float v) { append(&v, sizeof(v)); }
    void bytes(const void* data, size_t size) { append(data, size); }
    const std::vector<uint8_t>& data() const { return mData; }

private:
    void append(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        mData.insert(mData.end(), p, p + size);
    }

    std::vector<uint8_t> mData;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : mData(data), mSize(size) {}

    template <typename T>
    T read() {
        T v{};
        if (mPos + sizeof(T) <= mSize) {
            std::memcpy(&v, mData + mPos, sizeof(T));
        }
        mPos += sizeof(T);
        return v;
    }

private:
    const uint8_t* mData;
    size_t mSize;
    size_t mPos = 0;
};

std::vector<uint8_t> encode_tick(const TickRecord& r) {
    ByteWriter w;
    w.u32(r.tick);
    w.u32(r.flags);
    for (const auto& pad : r.input.pad) {
        w.u16(pad.button);
        w.u8(static_cast<uint8_t>(pad.stickX));
        w.u8(static_cast<uint8_t>(pad.stickY));
        w.u8(static_cast<uint8_t>(pad.substickX));
        w.u8(static_cast<uint8_t>(pad.substickY));
        w.u8(pad.triggerLeft);
        w.u8(pad.triggerRight);
        w.u8(pad.analogA);
        w.u8(pad.analogB);
        w.u8(static_cast<uint8_t>(pad.err));
        w.u8(0);
        w.u32(pad.extButton);
    }
    w.u32(r.input.actionBits);
    for (float v : r.input.mouse) {
        w.f32(v);
    }
    for (float v : r.input.gyro) {
        w.f32(v);
    }
    for (float v : r.input.touch) {
        w.f32(v);
    }
    w.u64(r.stateHash);
    for (int32_t v : r.rnd) {
        w.s32(v);
    }
    w.u32(r.rndCalls);
    w.u32(r.presRndCalls);
    w.u32(r.input.rumbleSupported);
    return w.data();
}

TickRecord decode_tick(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    TickRecord rec{};
    rec.tick = r.read<uint32_t>();
    rec.flags = r.read<uint32_t>();
    for (auto& pad : rec.input.pad) {
        pad.button = r.read<uint16_t>();
        pad.stickX = r.read<int8_t>();
        pad.stickY = r.read<int8_t>();
        pad.substickX = r.read<int8_t>();
        pad.substickY = r.read<int8_t>();
        pad.triggerLeft = r.read<uint8_t>();
        pad.triggerRight = r.read<uint8_t>();
        pad.analogA = r.read<uint8_t>();
        pad.analogB = r.read<uint8_t>();
        pad.err = r.read<int8_t>();
        r.read<uint8_t>();
        pad.extButton = r.read<uint32_t>();
    }
    rec.input.actionBits = r.read<uint32_t>();
    for (float& v : rec.input.mouse) {
        v = r.read<float>();
    }
    for (float& v : rec.input.gyro) {
        v = r.read<float>();
    }
    for (float& v : rec.input.touch) {
        v = r.read<float>();
    }
    rec.stateHash = r.read<uint64_t>();
    for (int32_t& v : rec.rnd) {
        v = r.read<int32_t>();
    }
    rec.rndCalls = r.read<uint32_t>();
    rec.presRndCalls = r.read<uint32_t>();
    rec.input.rumbleSupported = r.read<uint32_t>();
    rec.input.resetSwitch = (rec.flags & TickResetSwitch) != 0;
    rec.input.gyroReset = (rec.flags & TickGyroReset) != 0;
    return rec;
}

std::vector<uint8_t> encode_anchor(const AnchorSnapshot& a) {
    ByteWriter w;
    w.u32(1);
    for (int32_t v : a.rnd) {
        w.s32(v);
    }
    for (int32_t v : a.rnd2) {
        w.s32(v);
    }
    w.u32(a.counter0);
    w.s32(a.counter1);
    w.u32(a.timer);
    w.u64(a.totalTicks);
    w.u64(a.anchorHash);
    return w.data();
}

AnchorSnapshot decode_anchor(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    AnchorSnapshot a{};
    r.read<uint32_t>();
    for (int32_t& v : a.rnd) {
        v = r.read<int32_t>();
    }
    for (int32_t& v : a.rnd2) {
        v = r.read<int32_t>();
    }
    a.counter0 = r.read<uint32_t>();
    a.counter1 = r.read<int32_t>();
    a.timer = r.read<uint32_t>();
    a.totalTicks = r.read<uint64_t>();
    a.anchorHash = r.read<uint64_t>();
    return a;
}

void write_chunk(uint32_t tag, const void* payload, size_t size) {
    if (!s.out.is_open()) {
        return;
    }
    const uint32_t size32 = static_cast<uint32_t>(size);
    s.out.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
    s.out.write(reinterpret_cast<const char*>(&size32), sizeof(size32));
    if (size != 0) {
        s.out.write(static_cast<const char*>(payload), static_cast<std::streamsize>(size));
    }
    if (!s.out) {
        DuskLog.error("input recording: write to {} failed; recording stopped", s.file);
        s.out.close();
        s.mode = Mode::Idle;
        set_gate_enabled(false);
    }
}

void write_chunk(uint32_t tag, const std::vector<uint8_t>& payload) {
    write_chunk(tag, payload.data(), payload.size());
}

TickStateV1 collect_state(uint32_t tick, uint32_t rndCalls) {
    TickStateV1 state{};
    state.tick = tick;
    cM_getRndState(state.rnd);
    state.rndCalls = rndCalls;
    state.counter0 = g_Counter.mCounter0;
    state.timeOfDay = dComIfGs_getTime();
    if (const char* stage = dComIfGp_getStartStageName()) {
        std::strncpy(state.stage, stage, sizeof(state.stage));
    }
    state.room = -1;
    if (fopAc_ac_c* player = dComIfGp_getPlayer(0)) {
        state.room = player->current.roomNo;
        state.pos[0] = player->current.pos.x;
        state.pos[1] = player->current.pos.y;
        state.pos[2] = player->current.pos.z;
        state.angleY = player->shape_angle.y;
        state.speedF = player->speedF;
        state.playerPresent = 1;
    }
    if (dCamera_c* camera = dCam_getBody()) {
        const cXyz eye = camera->iEye();
        const cXyz center = camera->iCenter();
        state.eye[0] = eye.x;
        state.eye[1] = eye.y;
        state.eye[2] = eye.z;
        state.center[0] = center.x;
        state.center[1] = center.y;
        state.center[2] = center.z;
    }
    return state;
}

uint64_t hash_state(const TickStateV1& state) {
    return XXH64(&state, sizeof(state), 0);
}

std::string stage_name(const TickStateV1& state) {
    return std::string(state.stage, strnlen(state.stage, sizeof(state.stage)));
}

std::string describe_state(const TickStateV1& state, uint64_t hash, std::string_view tick) {
    return fmt::format(
        FMT_STRING("hash={:016x} tick={} rnd=({},{},{}) calls={} ctr={} stage={} "
                   "room={} pos=({:.3f},{:.3f},{:.3f}) angle={} eye=({:.3f},{:.3f},{:.3f})"),
        hash, tick, state.rnd[0], state.rnd[1], state.rnd[2], state.rndCalls, state.counter0,
        stage_name(state), state.room, state.pos[0], state.pos[1], state.pos[2], state.angleY,
        state.eye[0], state.eye[1], state.eye[2]);
}

std::string describe_state(const TickStateV1& state, uint64_t hash) {
    return describe_state(state, hash, fmt::to_string(state.tick));
}

void write_hash_log_row(const TickStateV1& state, uint64_t hash, uint32_t flags) {
    if (!s.hashLog.is_open()) {
        return;
    }
    int32_t rnd2[3];
    cM_getRnd2State(rnd2);
    s.hashLog << fmt::format(
        FMT_STRING("{},{:016x},{},{},{},{},{},{},{},{},{},{},{},{:.9g},{:.9g},"
                   "{:.9g},{},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},"
                   "{:.9g},{}\n"),
        state.tick, hash, state.rnd[0], state.rnd[1], state.rnd[2], state.rndCalls, s.presRnd,
        rnd2[0], rnd2[1], rnd2[2], state.counter0, stage_name(state), state.room, state.pos[0],
        state.pos[1], state.pos[2], state.angleY, state.speedF, state.eye[0], state.eye[1],
        state.eye[2], state.center[0], state.center[1], state.center[2], state.timeOfDay, flags);
    if (++s.hashLogRows % kHashLogFlushRows == 0) {
        s.hashLog.flush();
    }
}

void open_hash_log() {
    if (s.options.hashLogPath.empty()) {
        return;
    }
    s.hashLog.open(
        borealis::io::fs_path_from_utf8(s.options.hashLogPath), std::ios::binary | std::ios::trunc);
    if (!s.hashLog) {
        DuskLog.warn("input replay: cannot open hash log {}", s.options.hashLogPath);
        return;
    }
    s.hashLog << "tick,hash,rnd0,rnd1,rnd2,rnd_calls,pres_rnd_calls,rnd2_0,rnd2_1,rnd2_2,counter0,"
                 "stage,room,pos_x,pos_y,pos_z,angle_y,speed_f,eye_x,eye_y,eye_z,ctr_x,ctr_y,"
                 "ctr_z,time_of_day,flags\n";
}

bool fail_replay(std::string_view message) {
    DuskLog.error("input replay: {}", message);
    s.exitCode = ExitReplayError;
    s.mode = Mode::Finished;
    set_gate_enabled(false);
    if (s.exitOnReplayEnd) {
        IsRunning = false;
    }
    return false;
}

bool load_replay(const std::string& path) {
    std::ifstream in(borealis::io::fs_path_from_utf8(path), std::ios::binary);
    if (!in) {
        DuskLog.error("input replay: cannot open {}", path);
        return false;
    }
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < kPrefixSize || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        DuskLog.error("input replay: {} is not an input recording", path);
        return false;
    }
    ByteReader prefix(data.data() + sizeof(kMagic), kPrefixSize - sizeof(kMagic));
    const auto formatVersion = prefix.read<uint16_t>();
    const auto prefixSize = prefix.read<uint16_t>();
    const auto headerSize = prefix.read<uint32_t>();
    if (formatVersion > kFormatVersion) {
        DuskLog.error("input replay: {} uses format {}, this build reads {}", path, formatVersion,
            kFormatVersion);
        return false;
    }
    if (prefixSize < kPrefixSize || size_t(prefixSize) + headerSize > data.size()) {
        DuskLog.error("input replay: {} has a corrupt header", path);
        return false;
    }
    const auto* headerBegin = reinterpret_cast<const char*>(data.data() + prefixSize);
    s.header = nlohmann::json::parse(headerBegin, headerBegin + headerSize, nullptr, false);
    if (s.header.is_discarded() || !s.header.is_object()) {
        DuskLog.error("input replay: {} has an unreadable header", path);
        return false;
    }
    const uint32_t tickRecordSize = s.header.value("tickRecordSize", kTickRecordSize);
    if (tickRecordSize < kTickRecordSize) {
        DuskLog.error("input replay: {} has {}-byte tick records", path, tickRecordSize);
        return false;
    }

    size_t pos = size_t(prefixSize) + headerSize;
    bool sawEnd = false;
    while (pos < data.size()) {
        if (data.size() - pos < 8) {
            DuskLog.warn("input replay: recording truncated");
            break;
        }
        ByteReader chunkHeader(data.data() + pos, 8);
        const auto tag = chunkHeader.read<uint32_t>();
        const auto size = chunkHeader.read<uint32_t>();
        pos += 8;
        if (data.size() - pos < size) {
            DuskLog.warn("input replay: recording truncated");
            break;
        }
        const uint8_t* payload = data.data() + pos;
        if (tag == kTagSave) {
            s.hasSave = true;
            s.save.assign(payload, payload + size);
        } else if (tag == kTagAnchor && size >= kAnchorSize) {
            s.anchorRecord = decode_anchor(payload, size);
        } else if (tag == kTagCommand && size >= 4) {
            const uint32_t tick = ByteReader(payload, 4).read<uint32_t>();
            s.commands.push_back(
                {tick, std::string(reinterpret_cast<const char*>(payload) + 4, size - 4)});
        } else if (tag == kTagTick && size >= kTickRecordSize) {
            TickRecord rec = decode_tick(payload, size);
            if (rec.tick != s.ticks.size()) {
                DuskLog.error("input replay: tick record {} found where {} was expected", rec.tick,
                    s.ticks.size());
                return false;
            }
            s.ticks.push_back(rec);
        } else if (tag == kTagEnd) {
            sawEnd = true;
        }
        pos += size;
    }
    if (!sawEnd) {
        DuskLog.warn("input replay: {} has no end marker", path);
    }
    if (!s.anchorRecord) {
        DuskLog.error("input replay: {} never reached its start point", path);
        return false;
    }

    const auto& launch = s.header["launch"];
    const int saveSlot = launch.is_object() ? launch.value("saveSlot", 0) : 0;
    if (saveSlot < 0 || saveSlot > 3) {
        DuskLog.error("input replay: {} names save slot {}", path, saveSlot);
        return false;
    }
    s.saveSlot = static_cast<uint8_t>(saveSlot);
    if (launch.is_object() && launch.contains("stage") && launch["stage"].is_object()) {
        const auto& stage = launch["stage"];
        s.stage = {stage.value("name", std::string{}), true,
            static_cast<s8>(stage.value("room", 0)), static_cast<s16>(stage.value("point", 0)),
            static_cast<s8>(stage.value("layer", -1))};
    }
    return true;
}

void run_replay_commands() {
    const CommandOutput output = [](std::string text) { DuskLog.info("[replay] {}", text); };
    while (s.nextCommand < s.commands.size() && s.commands[s.nextCommand].tick <= s.tick) {
        const std::string line = s.commands[s.nextCommand++].line;
        runCommand(line, s.replayCommands, output);
    }
}

void finish_replay() {
    s.mode = Mode::Finished;
    if (s.exitOnReplayEnd) {
        s.current = neutral_input();
    }
    if (s.divergentTicks != 0 && s.exitOnReplayEnd && s.exitCode == ExitOk) {
        s.exitCode = ExitDivergence;
    }
    const std::string first =
        s.firstDivergence ? fmt::format(FMT_STRING(" (first at {})"), *s.firstDivergence) : "";
    DuskLog.info("Replay finished: {} ticks, {} divergent ticks{}, presentation cM_rnd calls {}, "
                 "load barrier timeouts {}",
        s.tick, s.divergentTicks, first, s.presRndTotal, s.barrierTimeouts);
    if (s.hashLog.is_open()) {
        s.hashLog.flush();
    }
    if (!s.exitOnReplayEnd) {
        set_gate_enabled(false);
    }
}

// Loads queued in tick N finish before tick N+1, so completion never races the game's polling.
bool wait_for_loads() {
    bool timedOut = false;
    if (s_gateEnabled.load(std::memory_order_acquire)) {
        std::unique_lock lock(s_gateMutex);
        s_gateOpen = true;
        s_gateCv.notify_all();
        if (!s_pendingCv.wait_for(lock, kDvdBarrierTimeout,
                [] { return s_dvdPending.load(std::memory_order_acquire) <= 0; }))
        {
            DuskLog.warn(
                "input replay: DVD loads still pending after {} s", kDvdBarrierTimeout.count());
            timedOut = true;
        }
        s_gateOpen = false;
        if (!s_pendingCv.wait_for(lock, kWaveBarrierTimeout,
                [] { return s_wavePending.load(std::memory_order_acquire) <= 0; }))
        {
            DuskLog.warn(
                "input replay: wave loads still pending after {} s", kWaveBarrierTimeout.count());
            timedOut = true;
        }
    }
    return timedOut;
}

bool is_camera_pos(std::string_view line) {
    std::vector<std::string_view> words;
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t start = line.find_first_not_of(" \t", pos);
        if (start == std::string_view::npos) {
            break;
        }
        const size_t end = line.find_first_of(" \t", start);
        words.push_back(line.substr(start, end - start));
        pos = end;
    }
    return words.size() == 2 && words[0] == "camera" && words[1] == "pos";
}

void notify_pending_drained() {
    std::lock_guard lock(s_gateMutex);
    s_pendingCv.notify_all();
}

bool anchor_ready() {
    return dComIfGp_getPlayer(0) != nullptr && !fopOvlpM_IsDoingReq() && !mDoGph_gInf_c::isFade() &&
           !ui::is_prelaunch_open();
}

void anchor() {
    const TickStateV1 state = collect_state(0, 0);
    const uint64_t liveHash = hash_state(state);
    if (s.mode == Mode::Recording) {
        AnchorSnapshot snapshot{};
        cM_getRndState(snapshot.rnd);
        cM_getRnd2State(snapshot.rnd2);
        snapshot.counter0 = g_Counter.mCounter0;
        snapshot.counter1 = g_Counter.mCounter1;
        snapshot.timer = g_Counter.mTimer;
        snapshot.totalTicks = s_totalTicks;
        snapshot.anchorHash = liveHash;
        write_chunk(kTagAnchor, encode_anchor(snapshot));
        DuskLog.info("input recording: start point reached ({})", describe_state(state, liveHash));
    } else {
        const AnchorSnapshot& snapshot = *s.anchorRecord;
        if (liveHash == snapshot.anchorHash) {
            DuskLog.info("input replay: start point matches the recording");
        } else {
            DuskLog.warn(
                "input replay: start point differs: hash 0x{:016x} expected 0x{:016x} ({})",
                liveHash, snapshot.anchorHash, describe_state(state, liveHash));
        }
        // Forcing the snapshot repairs any boot variance the loading gate did not remove.
        cM_initRnd(snapshot.rnd[0], snapshot.rnd[1], snapshot.rnd[2]);
        cM_initRnd2(snapshot.rnd2[0], snapshot.rnd2[1], snapshot.rnd2[2]);
        g_Counter.mCounter0 = snapshot.counter0;
        g_Counter.mCounter1 = snapshot.counter1;
        g_Counter.mTimer = snapshot.timer;
        s_totalTicks = snapshot.totalTicks;
    }
    s.anchored = true;
    s.tick = 0;
}

void check_pinned_settings() {
    if (s.pinnedChangeWarned || s.tick % kPinnedCheckInterval != 0) {
        return;
    }
    if (pinned_settings() == s.pinned) {
        return;
    }
    s.pinnedChangeWarned = true;
    DuskLog.warn("input recording: a gameplay setting changed mid-recording; replays will diverge");
    ui::push_toast({
        .type = "warning",
        .title = "Input recording",
        .content = "A gameplay setting changed mid-recording; replays will diverge.",
        .duration = std::chrono::seconds(5),
    });
}

void finish_recording(const CommandOutput* output) {
    std::string line;
    if (!s.anchored) {
        line = "Recording cancelled before its start point";
    } else {
        ByteWriter end;
        end.u32(s.tick);
        end.u64(s.lastHash);
        end.u32(s.presRndTotal);
        write_chunk(kTagEnd, end.data());
        line = fmt::format(FMT_STRING("Recording saved: {} ({} ticks, {} commands, presentation "
                                      "cM_rnd calls {})"),
            s.file, s.tick, s.commandsRecorded, s.presRndTotal);
    }
    s.out.close();
    if (s.hashLog.is_open()) {
        s.hashLog.flush();
    }
    s.mode = Mode::Idle;
    set_gate_enabled(false);
    DuskLog.info("{}", line);
    if (output != nullptr) {
        (*output)(line);
    }
}

}  // namespace

bool configure(const Options& options) {
    s.options = options;
    if (!options.recordPath.empty() && !options.replayPath.empty()) {
        fprintf(stderr, "Argument Error: --record-input and --replay-input cannot be combined\n");
        return false;
    }
    if (options.recordPath.empty() && options.replayPath.empty()) {
        if (!options.hashLogPath.empty()) {
            DuskLog.warn("--input-hash-log needs --record-input or --replay-input; ignored");
        }
        return true;
    }
    s.exitOnReplayEnd = options.exitOnReplayEnd;

    if (!options.recordPath.empty()) {
        if (SaveRequested < 1 || SaveRequested > 3) {
            if (!StageRequested.set) {
                fprintf(stderr, "Argument Error: --record-input needs --load-save or --stage\n");
                return false;
            }
        }
        const auto recordPath = borealis::io::fs_path_from_utf8(options.recordPath);
        std::error_code ec;
        if (std::filesystem::exists(recordPath, ec)) {
            DuskLog.info("input recording: overwriting {}", options.recordPath);
        }
        s.out.open(recordPath, std::ios::binary | std::ios::trunc);
        if (!s.out) {
            fprintf(stderr, "Error: cannot create %s\n", options.recordPath.c_str());
            return false;
        }
        s.file = options.recordPath;
        s.saveSlot = (SaveRequested >= 1 && SaveRequested <= 3) ? SaveRequested : 0;
        s.stage = StageRequested;
        s.mode = Mode::Recording;
    } else {
        if ((SaveRequested >= 1 && SaveRequested <= 3) || StageRequested.set) {
            fprintf(stderr, "Argument Error: --replay-input takes its start point from the "
                            "recording; drop --load-save and --stage\n");
            return false;
        }
        s.file = options.replayPath;
        if (!load_replay(options.replayPath)) {
            s.exitCode = ExitReplayError;
            return false;
        }
        SaveRequested = s.saveSlot;
        StageRequested = s.stage;
        s.replayCommands.replaySource = true;
        s.mode = Mode::Replaying;
        DuskLog.info(
            "input replay: {} ({} ticks, {} commands)", s.file, s.ticks.size(), s.commands.size());
    }

    s_virtualTime = true;
    set_gate_enabled(true);
    return true;
}

void apply_setting_overrides() {
    if (!is_session_mode()) {
        return;
    }
    config::load_arg_override("game.enableMenuPointer", "false");
    config::load_arg_override("game.debugFlyCam", "false");
    config::load_arg_override("game.autoSave", "false");
    // Touch overlay state reaches action binds and the camera mid-tick, outside recorded input.
    config::load_arg_override("game.enableTouchControls", "false");
    if (s.mode != Mode::Replaying) {
        return;
    }
    config::load_arg_override("game.pauseOnFocusLost", "false");
    const auto& settings = s.header["settings"];
    if (!settings.is_object()) {
        return;
    }
    for (const auto& [name, value] : settings.items()) {
        if (!value.is_string()) {
            continue;
        }
        if (config::GetConfigVar(name) == nullptr) {
            DuskLog.warn("input replay: recording pins unknown setting {}; ignored", name);
            continue;
        }
        config::load_arg_override(name, value.get<std::string>());
    }
}

void on_game_start() {
    if (!is_session_mode()) {
        return;
    }
    if ((SaveRequested < 1 || SaveRequested > 3) && !StageRequested.set) {
        if (s.mode == Mode::Replaying) {
            fail_replay("no DVD image to start the recording's save or stage; replay disabled");
        } else {
            DuskLog.error("input recording: no DVD image to start the save or stage; recording "
                          "disabled");
            s.out.close();
            s.mode = Mode::Idle;
            set_gate_enabled(false);
        }
        return;
    }

    const auto gameVersion = static_cast<int>(version::getGameVersion());
    if (s.mode == Mode::Recording) {
        s.pinned = pinned_settings();
        nlohmann::json settings = nlohmann::json::object();
        for (const auto& [name, value] : s.pinned) {
            settings[name] = value;
        }
        nlohmann::json mods = nlohmann::json::array();
        for (const auto& mod : mods::ModLoader::instance().active_mods()) {
            mods.push_back({{"id", mod.metadata.id}, {"version", mod.metadata.version}});
        }
        nlohmann::json stage = nullptr;
        if (s.stage.set) {
            stage = {{"name", s.stage.stage}, {"room", (int)s.stage.room},
                {"point", (int)s.stage.point}, {"layer", (int)s.stage.layer}};
        }
        s.header = {
            {"format", 1},
            {"build", BOREALIS_APP_DESCRIBE},
            {"createdUtc", utc_now()},
            {"simHz", kSimHz},
            {"tickRecordSize", kTickRecordSize},
            {"launch", {{"saveSlot", s.saveSlot}, {"stage", stage}}},
            {"gameVersion", gameVersion},
            {"anchor", "gameplay"},
            {"settings", settings},
            {"mods", mods},
            {"frameInterpolation", (int)getSettings().game.enableFrameInterpolation.getValue()},
        };
        const std::string json = s.header.dump();
        ByteWriter prefix;
        prefix.bytes(kMagic, sizeof(kMagic));
        prefix.u16(kFormatVersion);
        prefix.u16(kPrefixSize);
        prefix.u32(static_cast<uint32_t>(json.size()));
        s.out.write(reinterpret_cast<const char*>(prefix.data().data()),
            static_cast<std::streamsize>(prefix.data().size()));
        s.out.write(json.data(), static_cast<std::streamsize>(json.size()));
        DuskLog.info("input recording: {} (waiting for the start point)", s.file);
    } else {
        if (s.header.value("gameVersion", -1) != gameVersion) {
            fail_replay(fmt::format(FMT_STRING("recorded on game version {}, running {}"),
                s.header.value("gameVersion", -1), gameVersion));
            return;
        }
        if (s.header.value("build", std::string{}) != BOREALIS_APP_DESCRIBE) {
            DuskLog.warn("input replay: recorded with build {}, running {}",
                s.header.value("build", std::string{}), BOREALIS_APP_DESCRIBE);
        }
        nlohmann::json mods = nlohmann::json::array();
        for (const auto& mod : mods::ModLoader::instance().active_mods()) {
            mods.push_back({{"id", mod.metadata.id}, {"version", mod.metadata.version}});
        }
        if (s.header.contains("mods") && s.header["mods"] != mods) {
            DuskLog.warn("input replay: active mods differ from the recording");
        }
    }
    open_hash_log();
}

void before_frame() {
    if (s.mode == Mode::Replaying && s.anchored) {
        run_replay_commands();
    }
    if (s.mode == Mode::Replaying || s.mode == Mode::Finished) {
        pollCommands(s.replayCommands, [](std::string text) { DuskLog.info("[replay] {}", text); });
    }
    if (holds_neutral_input() && !isRenderTraceActive()) {
        IsRunning = false;
    }
}

void begin_tick() {
    if (s_virtualTime) {
        s_virtualNow = static_cast<OSTime>(s_totalTicks) * kTicksPerSimTick;
    }
    if (!is_session_mode()) {
        if (holds_neutral_input()) {
            wait_for_loads();
            s.current = neutral_input();
            touch_camera::set_pending(0.0f, 0.0f);
        }
        if (s_virtualTime) {
            ++s_totalTicks;
        }
        return;
    }

    s.presRnd = cM_getRndCallCount() - s.rndAtLastTickEnd;
    if (s.mode == Mode::Replaying && s.anchored) {
        run_replay_commands();
    }
    s.currentFlags = 0;
    if (wait_for_loads()) {
        s.currentFlags |= TickLoadBarrierTimeout;
        ++s.barrierTimeouts;
    }

    if (!s.anchored && anchor_ready()) {
        anchor();
    }
    if (!s.anchored && ++s.preAnchorTicks > kPreAnchorTickLimit) {
        if (s.mode == Mode::Replaying) {
            fail_replay("start point not reached after 9000 ticks");
        } else {
            DuskLog.error("input recording: start point not reached after 9000 ticks; recording "
                          "cancelled");
            finish_recording(nullptr);
        }
        ++s_totalTicks;
        return;
    }

    if (!s.anchored) {
        s.current = neutral_input();
    } else if (s.mode == Mode::Replaying) {
        if (s.tick >= s.ticks.size()) {
            finish_replay();
            ++s_totalTicks;
            return;
        }
        s.current = s.ticks[s.tick].input;
    } else {
        s.current = {};
    }

    if (s.mode == Mode::Recording && s.anchored) {
        touch_camera::get_pending(s.current.touch[0], s.current.touch[1]);
    } else {
        touch_camera::set_pending(s.current.touch[0], s.current.touch[1]);
    }

    s_virtualNow = static_cast<OSTime>(s_totalTicks) * kTicksPerSimTick;
    ++s_totalTicks;
    s_inTick.store(true, std::memory_order_relaxed);
    s.rndAtTickStart = cM_getRndCallCount();
}

void end_tick() {
    if (!is_session_mode()) {
        return;
    }
    const uint32_t calls = cM_getRndCallCount() - s.rndAtTickStart;
    const TickStateV1 state = collect_state(s.tick, calls);
    const uint64_t hash = hash_state(state);
    const bool watchdogFired = s_gateWatchdogFired.exchange(false, std::memory_order_relaxed);

    if (s.anchored) {
        uint32_t flags = s.currentFlags;
        if (s.current.resetSwitch) {
            flags |= TickResetSwitch;
        }
        if (s.current.gyroReset) {
            flags |= TickGyroReset;
        }
        if (watchdogFired) {
            flags |= TickDvdGateWatchdog;
        }
        if (s.mode == Mode::Recording) {
            TickRecord rec{};
            rec.tick = s.tick;
            rec.flags = flags;
            rec.input = s.current;
            rec.stateHash = hash;
            std::memcpy(rec.rnd, state.rnd, sizeof(rec.rnd));
            rec.rndCalls = calls;
            rec.presRndCalls = s.presRnd;
            write_chunk(kTagTick, encode_tick(rec));
        } else if (s.mode == Mode::Replaying) {
            const TickRecord& expected = s.ticks[s.tick];
            if (hash != expected.stateHash) {
                if (!s.firstDivergence) {
                    s.firstDivergence = s.tick;
                    DuskLog.warn(
                        "input replay: divergence at tick {}: hash 0x{:016x} expected 0x{:016x}; "
                        "rnd ({},{},{})/{} expected ({},{},{})/{}; presentation cM_rnd "
                        "calls {}; {}",
                        s.tick, hash, expected.stateHash, state.rnd[0], state.rnd[1], state.rnd[2],
                        calls, expected.rnd[0], expected.rnd[1], expected.rnd[2], expected.rndCalls,
                        s.presRnd, describe_state(state, hash));
                }
                ++s.divergentTicks;
            }
        }
        write_hash_log_row(state, hash, flags);
        s.lastHash = hash;
        s.lastState = state;
        s.hasLastState = true;
        s.presRndTotal += s.presRnd;
        ++s.tick;
        if (s.mode == Mode::Recording) {
            check_pinned_settings();
        } else if (s.mode == Mode::Replaying && s.tick >= s.ticks.size()) {
            finish_replay();
        }
    }
    s.rndAtLastTickEnd = cM_getRndCallCount();
    s_inTick.store(false, std::memory_order_relaxed);
}

void shutdown() {
    if (s.mode == Mode::Recording) {
        finish_recording(nullptr);
    } else if (s.mode == Mode::Replaying) {
        DuskLog.warn("input replay: stopped at tick {} of {}", s.tick, s.ticks.size());
        finish_replay();
    }
    if (s.hashLog.is_open()) {
        s.hashLog.close();
    }
    set_gate_enabled(false);
}

int exit_code() {
    return s.exitCode;
}

Mode mode() {
    return s.mode;
}

Position position() {
    return {s.mode, s.file, s.tick, s.anchored};
}

TickInput* override_input() {
    if (holds_neutral_input()) {
        return &s.current;
    }
    if (s.mode == Mode::Replaying || (s.mode == Mode::Recording && !s.anchored)) {
        return &s.current;
    }
    return nullptr;
}

TickInput* capture_input() {
    return (s.mode == Mode::Recording && s.anchored) ? &s.current : nullptr;
}

void filter_pad_status(PADStatus* status, bool& resetSwitch, uint32_t& rumbleSupported) {
    if (TickInput* in = override_input()) {
        std::memcpy(status, in->pad, sizeof(in->pad));
        resetSwitch = in->resetSwitch;
        rumbleSupported = in->rumbleSupported;
    } else if (TickInput* cap = capture_input()) {
        std::memcpy(cap->pad, status, sizeof(cap->pad));
        cap->resetSwitch = resetSwitch;
        cap->rumbleSupported = rumbleSupported;
    }
}

uint32_t filter_action_mask(uint32_t port, uint32_t mask) {
    if (TickInput* in = override_input()) {
        return (mask & ~0x0Fu) | ((in->actionBits >> (port * 8)) & 0x0Fu);
    }
    if (TickInput* cap = capture_input()) {
        cap->actionBits |= (mask & 0x0Fu) << (port * 8);
    }
    return mask;
}

uint8_t filter_loaded_save(uint8_t* cardBuffer, uint32_t slot, uint8_t status) {
    if (s.mode == Mode::Recording) {
        if (status == 1) {
            // Embedding the save keeps replays independent of the user's memory card.
            write_chunk(kTagSave, cardBuffer + slot * kSaveSize, kSaveSize);
        } else {
            write_chunk(kTagSave, nullptr, 0);
        }
        return status;
    }
    if (s.mode != Mode::Replaying) {
        return status;
    }
    if (!s.hasSave) {
        if (s.saveSlot != 0) {
            DuskLog.warn("input replay: recording has no embedded save; starting a new file");
        }
        return 2;
    }
    if (s.save.size() == kSaveSize) {
        std::memcpy(cardBuffer + slot * kSaveSize, s.save.data(), kSaveSize);
        return 1;
    }
    return 2;
}

bool virtual_os_time(OSTime& out) {
    if (!s_virtualTime) {
        return false;
    }
    out = s_virtualNow;
    return true;
}

void dvd_command_queued() {
    s_dvdPending.fetch_add(1, std::memory_order_acq_rel);
}

void dvd_command_finished() {
    if (s_dvdPending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        notify_pending_drained();
    }
}

void dvd_gate_wait() {
    if (!s_gateEnabled.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lock(s_gateMutex);
    const bool opened = s_gateCv.wait_for(lock, kGateWatchdog,
        [] { return s_gateOpen || !s_gateEnabled.load(std::memory_order_relaxed); });
    if (!opened && s_inTick.load(std::memory_order_relaxed)) {
        s_gateWatchdogFired.store(true, std::memory_order_relaxed);
    }
}

void wave_load_queued() {
    s_wavePending.fetch_add(1, std::memory_order_acq_rel);
}

void wave_load_finished() {
    if (s_wavePending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        notify_pending_drained();
    }
}

bool accept_console_command(
    std::string_view cmd, std::string_view line, bool fromReplay, const CommandOutput& output) {
    if (fromReplay || cmd.starts_with("input.")) {
        return true;
    }
    if (s.mode == Mode::Recording) {
        if (!s.anchored) {
            output("Error: input recording has not reached its start point yet");
            return false;
        }
        ByteWriter w;
        w.u32(s.tick);
        w.bytes(line.data(), line.size());
        write_chunk(kTagCommand, w.data());
        ++s.commandsRecorded;
        return true;
    }
    if (s.mode == Mode::Replaying) {
        constexpr std::array<std::string_view, 7> kAllowed{
            "help", "pos", "list", "find", "echo", "rate", "render.trace"};
        if (std::find(kAllowed.begin(), kAllowed.end(), cmd) != kAllowed.end()) {
            return true;
        }
        if (cmd == "camera" && is_camera_pos(line)) {
            return true;
        }
        output(fmt::format(
            FMT_STRING("Error: '{}' is disabled during input replay (use input.stop)"), cmd));
        return false;
    }
    return true;
}

void print_status(const CommandOutput& output) {
    switch (s.mode) {
    case Mode::Recording:
        output(s.anchored ?
                   fmt::format(FMT_STRING("Recording {}: tick {}"), s.file, s.tick) :
                   fmt::format(FMT_STRING("Recording {}: waiting for start point"), s.file));
        break;
    case Mode::Replaying:
        output(fmt::format(FMT_STRING("Replaying {}: tick {}/{}, {} divergent ticks"), s.file,
            s.tick, s.ticks.size(), s.divergentTicks));
        break;
    case Mode::Finished:
        output(fmt::format(FMT_STRING("Replay finished: {} ticks, {} divergent ticks{}"), s.tick,
            s.divergentTicks,
            s.firstDivergence ? fmt::format(FMT_STRING(" (first at {})"), *s.firstDivergence) :
                                std::string{}));
        break;
    case Mode::Idle:
        output("Input replay: idle");
        break;
    }
    output(fmt::format(FMT_STRING("presentation cM_rnd calls: {}"), s.presRndTotal));
}

void print_hash(const CommandOutput& output) {
    if (s.hasLastState && s.mode != Mode::Idle) {
        output(describe_state(s.lastState, s.lastHash));
        return;
    }
    const TickStateV1 state = collect_state(0, 0);
    output(describe_state(state, hash_state(state), "-"));
}

void stop(const CommandOutput& output) {
    if (s.mode == Mode::Recording) {
        finish_recording(&output);
        return;
    }
    if (s.mode == Mode::Replaying) {
        const std::string line =
            fmt::format(FMT_STRING("Replay stopped at tick {}; live input restored"), s.tick);
        s.mode = Mode::Idle;
        s.exitOnReplayEnd = false;
        set_gate_enabled(false);
        DuskLog.info("{}", line);
        output(line);
        return;
    }
    output("No input recording or replay in progress");
}

}  // namespace dusk::input_replay
