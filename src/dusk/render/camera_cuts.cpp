#include "dusk/render/camera_cuts.hpp"

#include "dusk/interp/frame_interpolation.h"
#include "dusk/render/hints.hpp"
#include "dusk/render/model_registry.hpp"
#include "dusk/render/render_bridge.hpp"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_manager.h"

#include <aurora/gfx.hpp>
#include <borealis/log.hpp>
#include <dolphin/gx/GXAurora.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace dusk::render {
namespace {

constexpr borealis::Log CutLog{"dusk::render"};

static_assert(AURORA_OBJ_TELEPORT == 1u << 1);
static_assert(kCutSourceCount == 5);
static_assert(static_cast<size_t>(CutSource::Jump) + 1 == kCutSourceCount);

struct CameraPose {
    cXyz eye{0.0f, 0.0f, 0.0f};
    cXyz center{0.0f, 0.0f, -1.0f};
    f32 fovy = 0.0f;
    s16 bank = 0;
};

struct PoseLimits {
    f32 distance;
    f32 minDirectionCos;
    f32 fovyDegrees;
};

constexpr PoseLimits kResetLimits{3.0f * kUnitsPerMeter, 0.8660254f, 10.0f};
constexpr PoseLimits kJumpLimits{10.0f * kUnitsPerMeter, 0.5f, 20.0f};
constexpr int kResetLatches = 2;
constexpr size_t kMaxTeleports = 64;
constexpr double kDegreesPerRadian = 57.29577951308232;

constexpr uint8_t source_bit(CutSource source) noexcept {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(source));
}

constexpr uint32_t next_epoch(uint32_t epoch) noexcept {
    return epoch == UINT32_MAX ? 1 : epoch + 1;
}

std::string_view source_name(CutSource source) noexcept {
    switch (source) {
    case CutSource::PresentationSync:
        return "presentation sync";
    case CutSource::CameraReset:
        return "camera reset";
    case CutSource::Warp:
        return "warp";
    case CutSource::NewCamera:
        return "new camera";
    case CutSource::Jump:
        return "jump";
    }
    return "?";
}

bool finite(const cXyz& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const CameraPose& pose) noexcept {
    return finite(pose.eye) && finite(pose.center) && std::isfinite(pose.fovy);
}

double eye_distance(const CameraPose& from, const CameraPose& to) noexcept {
    const double dx = double(to.eye.x) - from.eye.x;
    const double dy = double(to.eye.y) - from.eye.y;
    const double dz = double(to.eye.z) - from.eye.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A view with no direction cannot have turned, so it reads as no rotation
double direction_cos(const CameraPose& from, const CameraPose& to) noexcept {
    const double ax = double(from.center.x) - from.eye.x;
    const double ay = double(from.center.y) - from.eye.y;
    const double az = double(from.center.z) - from.eye.z;
    const double bx = double(to.center.x) - to.eye.x;
    const double by = double(to.center.y) - to.eye.y;
    const double bz = double(to.center.z) - to.eye.z;
    const double lengths = std::sqrt((ax * ax + ay * ay + az * az) * (bx * bx + by * by + bz * bz));
    if (lengths == 0.0) {
        return 1.0;
    }
    return std::clamp((ax * bx + ay * by + az * bz) / lengths, -1.0, 1.0);
}

double bank_step(const CameraPose& from, const CameraPose& to) noexcept {
    const auto step = static_cast<s16>(static_cast<u16>(to.bank) - static_cast<u16>(from.bank));
    return std::abs(step * (360.0 / 65536.0));
}

bool exceeds(const CameraPose& from, const CameraPose& to, const PoseLimits& limits) noexcept {
    if (!finite(from) || !finite(to)) {
        return true;
    }
    return eye_distance(from, to) > limits.distance ||
           direction_cos(from, to) < limits.minDirectionCos ||
           std::fabs(double(to.fovy) - from.fovy) > limits.fovyDegrees;
}

CameraPose pose_of(const view_class& view) noexcept {
    return {view.lookat.eye, view.lookat.center, view.fovy, view.bank};
}

class CutTracker {
public:
    CutTracker() = default;
    explicit CutTracker(uint32_t epoch) noexcept : m_epoch(epoch) {}

    void note(CutSource source) noexcept { m_pending |= source_bit(source); }

    void flag_reset() noexcept { m_resetLatchesLeft = kResetLatches; }

    bool comparable(uint32_t cameraId) const noexcept {
        return m_lastLatchValid && m_latchCameraId == cameraId;
    }

    // Returns the sources this latch hands to the next presentation
    uint8_t latch(uint32_t cameraId, const CameraPose& stored) noexcept {
        const bool same = comparable(cameraId);
        if (m_resetLatchesLeft > 0) {
            if (same && exceeds(m_lastLatch, stored, kResetLimits)) {
                note(CutSource::CameraReset);
                m_resetLatchesLeft = 0;
            } else {
                --m_resetLatchesLeft;
            }
        }
        // Tick poses, so a cut counts the same however many presentations sample it
        if (m_pending == 0 && same && exceeds(m_lastLatch, stored, kJumpLimits)) {
            note(CutSource::Jump);
        }
        m_lastLatch = stored;
        m_lastLatchValid = true;
        m_latchCameraId = cameraId;
        const uint8_t added = m_pending;
        m_latched |= m_pending;
        m_pending = 0;
        return added;
    }

    void begin_frame() noexcept {
        if (!m_cameraThisFrame) {
            m_prevValid = false;
        }
        m_cameraThisFrame = false;
    }

    uint32_t present(uint32_t cameraId, uint8_t& sources) noexcept {
        m_cameraThisFrame = true;
        const bool newCamera = !m_prevValid || cameraId != m_cameraId;
        sources = m_latched | (newCamera ? source_bit(CutSource::NewCamera) : 0);
        if (sources != 0) {
            m_epoch = next_epoch(m_epoch);
        }
        m_latched = 0;
        m_cameraId = cameraId;
        m_prevValid = true;
        return m_epoch;
    }

    uint32_t epoch() const noexcept { return m_epoch; }
    uint8_t pending() const noexcept { return m_pending; }
    uint8_t latched() const noexcept { return m_latched; }
    int reset_latches_left() const noexcept { return m_resetLatchesLeft; }
    const CameraPose& last_latch() const noexcept { return m_lastLatch; }

private:
    uint8_t m_pending = 0;
    uint8_t m_latched = 0;
    int m_resetLatchesLeft = 0;
    CameraPose m_lastLatch{};
    bool m_lastLatchValid = false;
    uint32_t m_latchCameraId = 0;
    uint32_t m_epoch = 0;
    uint32_t m_cameraId = 0;
    bool m_prevValid = false;
    bool m_cameraThisFrame = false;
};

class SceneEpoch {
public:
    SceneEpoch() = default;
    explicit SceneEpoch(uint32_t value) noexcept : m_value(value) {}

    void note() noexcept { m_pending = true; }

    uint32_t advance() noexcept {
        if (m_pending) {
            m_value = next_epoch(m_value);
            m_framesSince = 0;
            m_pending = false;
        } else {
            ++m_framesSince;
        }
        return m_value;
    }

    uint32_t value() const noexcept { return m_value; }
    uint32_t frames_since_change() const noexcept { return m_framesSince; }

private:
    bool m_pending = false;
    uint32_t m_value = 1;
    uint32_t m_framesSince = 0;
};

// Fixed storage, so a burst of forgotten matrices never allocates on the game thread
class TeleportQueue {
public:
    bool note(const J3DModel* model) noexcept {
        if (m_pendingCount == kMaxTeleports) {
            return false;
        }
        m_pending[m_pendingCount++] = model;
        return true;
    }

    size_t latch() noexcept {
        const size_t room = kMaxTeleports - m_latchedCount;
        const size_t moved = std::min(room, m_pendingCount);
        std::copy_n(m_pending.data(), moved, m_latched.data() + m_latchedCount);
        m_latchedCount += moved;
        const size_t dropped = m_pendingCount - moved;
        m_pendingCount = 0;
        return dropped;
    }

    template <typename Fn>
    size_t apply(Fn&& fn) {
        const size_t count = m_latchedCount;
        for (size_t i = 0; i < count; ++i) {
            fn(m_latched[i]);
        }
        m_latchedCount = 0;
        return count;
    }

    size_t latched_count() const noexcept { return m_latchedCount; }

private:
    std::array<const J3DModel*, kMaxTeleports> m_pending{};
    std::array<const J3DModel*, kMaxTeleports> m_latched{};
    size_t m_pendingCount = 0;
    size_t m_latchedCount = 0;
};

struct StepStats {
    double eye = 0.0;
    double direction = 0.0;
    double fovy = 0.0;
    double bank = 0.0;
};

struct CutState {
    CutTracker tracker;
    SceneEpoch scene;
    CutStats stats;
    TeleportQueue teleports;
    std::array<uint32_t, kCutSourceCount> runs{};
    StepStats steps;
    int suppression = 0;
    uint64_t cutFrames = 0;
    uint64_t stampPasses = 0;
};

CutState s_live;

std::string source_list(uint8_t bits) {
    std::string list;
    for (size_t i = 0; i < kCutSourceCount; ++i) {
        const auto source = static_cast<CutSource>(i);
        if ((bits & source_bit(source)) != 0) {
            if (!list.empty()) {
                list += ", ";
            }
            list += source_name(source);
        }
    }
    return list;
}

template <typename T>
std::string source_counts(const std::array<T, kCutSourceCount>& counts) {
    std::string text;
    for (size_t i = 0; i < kCutSourceCount; ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += fmt::format("{} {}", source_name(static_cast<CutSource>(i)), counts[i]);
    }
    return text;
}

void track_steps(StepStats& steps, const CameraPose& from, const CameraPose& to) noexcept {
    steps.eye = std::max(steps.eye, eye_distance(from, to));
    steps.direction =
        std::max(steps.direction, std::acos(direction_cos(from, to)) * kDegreesPerRadian);
    steps.fovy = std::max(steps.fovy, std::fabs(double(to.fovy) - from.fovy));
    steps.bank = std::max(steps.bank, bank_step(from, to));
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

CameraPose test_pose(f32 x, f32 yawDegrees, f32 fovy) noexcept {
    const double yaw = yawDegrees / kDegreesPerRadian;
    const auto dx = static_cast<f32>(std::sin(yaw) * 100.0);
    const auto dz = static_cast<f32>(-std::cos(yaw) * 100.0);
    return {{x, 0.0f, 0.0f}, {x + dx, 0.0f, dz}, fovy, 0};
}

std::string test_limits(const PoseLimits& limits, f32 distance, f32 degrees, f32 fovy) {
    std::string problems;
    const CameraPose base = test_pose(0.0f, 0.0f, 60.0f);
    const auto expect = [&](const CameraPose& to, bool cut, std::string_view what) {
        if (exceeds(base, to, limits) != cut) {
            append_problem(problems, fmt::format("{} {}", what, cut ? "not a cut" : "a cut"));
        }
    };
    expect(test_pose(distance - 1.0f, 0.0f, 60.0f), false, fmt::format("{} u", distance - 1.0f));
    expect(test_pose(distance + 1.0f, 0.0f, 60.0f), true, fmt::format("{} u", distance + 1.0f));
    expect(test_pose(0.0f, degrees - 1.0f, 60.0f), false, fmt::format("{} deg", degrees - 1.0f));
    expect(test_pose(0.0f, degrees + 1.0f, 60.0f), true, fmt::format("{} deg", degrees + 1.0f));
    expect(test_pose(0.0f, 0.0f, 60.0f + fovy - 0.1f), false, "fovy just under");
    expect(test_pose(0.0f, 0.0f, 60.0f + fovy + 0.1f), true, "fovy just over");
    CameraPose nan = base;
    nan.eye.x = std::nanf("");
    expect(nan, true, "NaN eye");
    CameraPose flat = test_pose(0.0f, 90.0f, 60.0f);
    flat.center = flat.eye;
    expect(flat, false, "zero-length direction");
    return problems;
}

std::string test_latch_once() {
    std::string problems;
    CutTracker tracker;
    const CameraPose pose = test_pose(0.0f, 0.0f, 60.0f);
    uint8_t sources = 0;
    tracker.present(7, sources);
    const uint32_t start = tracker.epoch();
    tracker.note(CutSource::Warp);
    tracker.latch(7, pose);
    if (tracker.epoch() != start) {
        append_problem(problems, "latch moved the epoch");
    }
    tracker.note(CutSource::PresentationSync);
    tracker.latch(7, pose);
    tracker.begin_frame();
    const uint32_t cut = tracker.present(7, sources);
    const uint8_t expected = source_bit(CutSource::Warp) | source_bit(CutSource::PresentationSync);
    if (cut != start + 1 || sources != expected) {
        append_problem(problems,
            fmt::format("two latches gave epoch +{} sources {:#x}", cut - start, sources));
    }
    tracker.begin_frame();
    if (tracker.present(7, sources) != cut || sources != 0) {
        append_problem(problems, "a presentation with nothing latched moved the epoch");
    }
    return problems;
}

std::string test_reset_window() {
    std::string problems;
    const CameraPose still = test_pose(0.0f, 0.0f, 60.0f);
    const CameraPose moved = test_pose(400.0f, 0.0f, 60.0f);
    CutTracker tracker;
    tracker.latch(1, still);
    tracker.flag_reset();
    tracker.latch(1, still);
    if (tracker.latched() != 0) {
        append_problem(problems, "unchanged pose noted a reset");
    }
    tracker.latch(1, moved);
    if (tracker.latched() != source_bit(CutSource::CameraReset)) {
        append_problem(problems, "400 u at the second latch is no reset");
    }
    CutTracker late;
    late.latch(1, still);
    late.flag_reset();
    late.latch(1, still);
    late.latch(1, still);
    late.latch(1, moved);
    if (late.latched() != 0) {
        append_problem(problems, "a move after the window noted a reset");
    }
    return problems;
}

std::string test_presentation() {
    std::string problems;
    CutTracker tracker;
    uint8_t sources = 0;
    const auto step = [&](uint32_t id, uint32_t expectedEpoch, uint8_t expectedSources,
                          std::string_view what) {
        tracker.begin_frame();
        const uint32_t epoch = tracker.present(id, sources);
        if (epoch != expectedEpoch || sources != expectedSources) {
            append_problem(
                problems, fmt::format("{}: epoch {} sources {:#x}", what, epoch, sources));
        }
    };
    const uint8_t newCamera = source_bit(CutSource::NewCamera);
    step(1, 1, newCamera, "first camera");
    step(1, 1, 0, "same camera");
    step(2, 2, newCamera, "new camera id");
    tracker.begin_frame();
    step(2, 3, newCamera, "after a frame without a camera");
    tracker.note(CutSource::Warp);
    tracker.latch(2, test_pose(0.0f, 0.0f, 60.0f));
    step(2, 4, source_bit(CutSource::Warp), "latched warp");
    step(2, 4, 0, "nothing latched");
    return problems;
}

std::string test_latch_jump() {
    std::string problems;
    const CameraPose start = test_pose(0.0f, 0.0f, 60.0f);
    const CameraPose far = test_pose(1001.0f, 0.0f, 60.0f);
    const uint8_t jump = source_bit(CutSource::Jump);
    CutTracker tracker;
    uint8_t sources = 0;
    tracker.present(1, sources);
    tracker.latch(1, start);
    if (tracker.latch(1, test_pose(999.0f, 0.0f, 60.0f)) != 0) {
        append_problem(problems, "a 999 u tick step is a jump");
    }
    if (tracker.latch(1, test_pose(2000.0f, 0.0f, 60.0f)) != jump) {
        append_problem(problems, "a 1001 u tick step is no jump");
    }
    tracker.begin_frame();
    if (tracker.present(1, sources) != 2 || sources != jump) {
        append_problem(problems, "the next presentation does not carry the jump");
    }
    tracker.note(CutSource::Warp);
    if (tracker.latch(1, start) != source_bit(CutSource::Warp)) {
        append_problem(problems, "a jump is added to a warp");
    }
    if (tracker.latch(2, far) != 0) {
        append_problem(problems, "a jump across two cameras");
    }
    return problems;
}

std::string test_wrap() {
    std::string problems;
    CutTracker tracker(UINT32_MAX);
    uint8_t sources = 0;
    if (tracker.present(1, sources) != 1) {
        append_problem(problems, "cut epoch does not wrap to 1");
    }
    SceneEpoch scene(UINT32_MAX);
    scene.note();
    if (scene.advance() != 1) {
        append_problem(problems, "scene epoch does not wrap to 1");
    }
    return problems;
}

std::string test_scene_epoch() {
    std::string problems;
    SceneEpoch scene;
    scene.note();
    scene.note();
    if (scene.advance() != 2 || scene.frames_since_change() != 0) {
        append_problem(problems, "two notes did not give one change");
    }
    if (scene.advance() != 2 || scene.frames_since_change() != 1) {
        append_problem(problems, "a frame without notes changed the epoch");
    }
    return problems;
}

std::string test_teleports() {
    std::string problems;
    TeleportQueue queue;
    size_t refused = 0;
    std::array<int, kMaxTeleports + 1> models{};
    for (const int& model : models) {
        refused += queue.note(reinterpret_cast<const J3DModel*>(&model)) ? 0 : 1;
    }
    const size_t dropped = queue.latch();
    if (refused != 1 || dropped != 0 || queue.latched_count() != kMaxTeleports) {
        append_problem(problems, fmt::format("65 notes: {} latched, {} refused, {} dropped",
                                     queue.latched_count(), refused, dropped));
    }
    queue.note(reinterpret_cast<const J3DModel*>(&models[0]));
    if (queue.latch() != 1) {
        append_problem(problems, "a full latched list took another model");
    }
    size_t applied = 0;
    queue.apply([&](const J3DModel*) { ++applied; });
    if (applied != kMaxTeleports || queue.latched_count() != 0) {
        append_problem(problems, fmt::format("applied {} stamps", applied));
    }
    return problems;
}

// The live functions run on a fresh state here, so a test never leaks a cut into the game
template <typename Fn>
std::string with_fresh_state(Fn&& fn) {
    const CutState saved = s_live;
    s_live = CutState{};
    std::string problems = fn();
    s_live = saved;
    return problems;
}

std::string test_suppression() {
    return with_fresh_state([] {
        std::string problems;
        {
            const ScopedCutSuppression outer;
            {
                const ScopedCutSuppression inner;
                note_camera_cut(CutSource::PresentationSync);
                note_camera_cut(CutSource::Warp);
            }
            note_camera_cut(CutSource::PresentationSync);
        }
        if (s_live.stats.suppressedSyncs != 2) {
            append_problem(
                problems, fmt::format("{} suppressed syncs counted", s_live.stats.suppressedSyncs));
        }
        if (s_live.tracker.pending() != source_bit(CutSource::Warp)) {
            append_problem(problems, "a warp under a guard is not pending");
        }
        if (s_live.suppression != 0) {
            append_problem(problems, fmt::format("depth {} after both guards", s_live.suppression));
        }
        note_camera_cut(CutSource::PresentationSync);
        if ((s_live.tracker.pending() & source_bit(CutSource::PresentationSync)) == 0) {
            append_problem(problems, "a sync after the guards is not pending");
        }
        return problems;
    });
}

// Only camera 0 latches and flags resets; the guards are why other cameras never cut the main view
std::string test_camera_zero() {
    return with_fresh_state([] {
        std::string problems;
        const camera_process_class* process = dComIfGp_getCamera(0);
        note_camera_cut(CutSource::Warp);
        latch_camera_cuts(1);
        if (s_live.tracker.pending() != source_bit(CutSource::Warp)) {
            append_problem(problems, "camera 1 latched the pending cuts");
        }
        latch_camera_cuts(0);
        if (s_live.tracker.latched() != source_bit(CutSource::Warp)) {
            append_problem(problems, "camera 0 did not latch");
        }
        alignas(dCamera_c) static const std::array<std::byte, sizeof(dCamera_c)> other{};
        note_camera_reset(*reinterpret_cast<const dCamera_c*>(other.data()));
        if (s_live.tracker.reset_latches_left() != 0) {
            append_problem(problems, "another camera flagged a reset");
        }
        note_camera_reset(process->mCamera);
        if (s_live.tracker.reset_latches_left() != kResetLatches) {
            append_problem(problems, "camera 0 did not flag a reset");
        }
        return problems;
    });
}

std::string test_epoch_sent() {
    return current_cut_epoch() == s_live.tracker.epoch() ?
               std::string{} :
               fmt::format("CAMERA carried {}, the tracker is at {}", current_cut_epoch(),
                   s_live.tracker.epoch());
}

std::string test_stamps_applied() {
    return s_live.stampPasses == s_live.cutFrames ?
               std::string{} :
               fmt::format(
                   "{} painter frames, {} stamp passes", s_live.cutFrames, s_live.stampPasses);
}

std::string test_funnel() {
    return with_fresh_state([] {
        std::string problems;
        dusk::interp::request_presentation_sync();
        if (s_live.tracker.pending() != source_bit(CutSource::PresentationSync)) {
            append_problem(problems, "request_presentation_sync noted no cut");
        }
        if (dusk::interp::presentation_sync_active()) {
            append_problem(problems, "the sync became active with interpolation off");
        }
        return problems;
    });
}

}  // namespace

void note_camera_cut(CutSource source) noexcept {
    if (source == CutSource::PresentationSync && s_live.suppression > 0) {
        ++s_live.stats.suppressedSyncs;
        return;
    }
    s_live.tracker.note(source);
}

void note_camera_reset(const dCamera_c& camera) noexcept {
    const camera_process_class* process = dComIfGp_getCamera(0);
    if (process != nullptr && &process->mCamera == &camera) {
        s_live.tracker.flag_reset();
    }
}

void latch_camera_cuts(int cameraId) noexcept {
    if (cameraId != 0) {
        return;
    }
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return;
    }
    const CameraPose pose = pose_of(camera->view);
    const uint32_t id = fpcM_GetID(camera);
    const bool comparable = s_live.tracker.comparable(id);
    const CameraPose previous = s_live.tracker.last_latch();
    if (s_live.tracker.latch(id, pose) == 0 && comparable) {
        track_steps(s_live.steps, previous, pose);
    }
    latch_teleports();
}

void latch_teleports() noexcept {
    s_live.stats.teleportDropped += s_live.teleports.latch();
}

void note_room_geometry(int roomNo, bool present) noexcept {
    s_live.scene.note();
    if (present) {
        ++s_live.stats.roomLoads;
    } else {
        ++s_live.stats.roomUnloads;
    }
    CutLog.debug("room {} geometry {}", roomNo, present ? "loaded" : "unloaded");
}

void note_teleport(const J3DModel* model) noexcept {
    if (model != nullptr && !s_live.teleports.note(model)) {
        ++s_live.stats.teleportDropped;
    }
}

ScopedCutSuppression::ScopedCutSuppression() noexcept {
    ++s_live.suppression;
}

ScopedCutSuppression::~ScopedCutSuppression() {
    --s_live.suppression;
}

void begin_cut_frame() noexcept {
    ++s_live.cutFrames;
    s_live.tracker.begin_frame();
}

void apply_teleport_stamps() noexcept {
    ++s_live.stampPasses;
    const size_t stamped = s_live.teleports.apply(&mark_model_teleport);
    if (stamped != 0) {
        s_live.stats.teleportStamps += stamped;
        CutLog.debug("teleport stamps {} at frame {}", stamped, aurora::gfx::current_frame());
    }
}

uint32_t advance_cut_epoch(const camera_process_class& camera) noexcept {
    uint8_t sources = 0;
    const uint32_t epoch = s_live.tracker.present(fpcM_GetID(&camera), sources);
    CutStats& stats = s_live.stats;
    for (size_t i = 0; i < kCutSourceCount; ++i) {
        const bool set = (sources & source_bit(static_cast<CutSource>(i))) != 0;
        s_live.runs[i] = set ? s_live.runs[i] + 1 : 0;
        stats.maxConsecutive[i] = std::max(stats.maxConsecutive[i], s_live.runs[i]);
        stats.bySource[i] += set ? 1 : 0;
    }
    if (sources != 0) {
        ++stats.cuts;
        CutLog.debug("camera cut {} at frame {}: {}", epoch, aurora::gfx::current_frame(),
            source_list(sources));
    }
    return epoch;
}

uint32_t cut_epoch() noexcept {
    return s_live.tracker.epoch();
}

uint32_t advance_scene_epoch() noexcept {
    const uint32_t before = s_live.scene.value();
    const uint32_t epoch = s_live.scene.advance();
    if (epoch != before) {
        ++s_live.stats.sceneChanges;
        CutLog.debug("scene epoch {} at frame {}", epoch, aurora::gfx::current_frame());
    }
    return epoch;
}

uint32_t scene_epoch() noexcept {
    return s_live.scene.value();
}

uint32_t frames_since_scene_change() noexcept {
    return s_live.scene.frames_since_change();
}

const CutStats& cut_stats() noexcept {
    return s_live.stats;
}

void reset_cut_stats() noexcept {
    s_live.stats = {};
    s_live.runs = {};
    s_live.steps = {};
}

std::vector<std::string> describe_cuts() {
    const CutStats& stats = s_live.stats;
    const StepStats& steps = s_live.steps;
    return {
        fmt::format("cuts: epoch {}, cut frames {} ({}), suppressed syncs {}",
            s_live.tracker.epoch(), stats.cuts, source_counts(stats.bySource),
            stats.suppressedSyncs),
        fmt::format("cuts: longest run ({}), teleport stamps {} dropped {}",
            source_counts(stats.maxConsecutive), stats.teleportStamps, stats.teleportDropped),
        fmt::format("cuts: largest step without a cut: eye {:.1f} u, direction {:.1f} deg, "
                    "fovy {:.1f} deg, bank {:.1f} deg",
            steps.eye, steps.direction, steps.fovy, steps.bank),
        fmt::format("scene: epoch {}, changes {}, rooms +{} -{}, frames since change {}",
            s_live.scene.value(), stats.sceneChanges, stats.roomLoads, stats.roomUnloads,
            s_live.scene.frames_since_change()),
    };
}

void cuts_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    check("cuts.reset_limits", test_limits(kResetLimits, 300.0f, 30.0f, 10.0f));
    check("cuts.jump_limits", test_limits(kJumpLimits, 1000.0f, 60.0f, 20.0f));
    check("cuts.latch_once", test_latch_once());
    check("cuts.reset_window", test_reset_window());
    check("cuts.presentation", test_presentation());
    check("cuts.latch_jump", test_latch_jump());
    check("cuts.wrap", test_wrap());
    check("cuts.scene_epoch", test_scene_epoch());
    check("cuts.teleports", test_teleports());
    check("cuts.suppression", test_suppression());
    if (dComIfGp_getCamera(0) == nullptr) {
        report.skipped.push_back("cuts.camera_zero: no camera 0");
    } else {
        check("cuts.camera_zero", test_camera_zero());
    }
    if (!hints_active() || s_live.tracker.epoch() == 0) {
        report.skipped.push_back("cuts.epoch_sent: no CAMERA hint sent yet");
    } else {
        check("cuts.epoch_sent", test_epoch_sent());
    }
    if (s_live.cutFrames == 0) {
        report.skipped.push_back("cuts.stamps_applied: no painter frame yet");
    } else {
        check("cuts.stamps_applied", test_stamps_applied());
    }
    // A real sync would snap the presented frame, so the funnel runs only while it is a no-op
    if (dusk::interp::is_enabled()) {
        report.skipped.push_back("cuts.funnel: frame interpolation is on");
    } else {
        check("cuts.funnel", test_funnel());
    }
}

}  // namespace dusk::render
