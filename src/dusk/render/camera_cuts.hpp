#pragma once

#include <dolphin/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class camera_process_class;
class dCamera_c;
class J3DModel;

namespace dusk::render {

struct SelfTestReport;

enum class CutSource : u8 {
    PresentationSync,
    CameraReset,
    Warp,
    NewCamera,
    Jump,
};
inline constexpr size_t kCutSourceCount = 5;

struct CutStats {
    uint64_t cuts = 0;
    std::array<uint64_t, kCutSourceCount> bySource{};
    std::array<uint32_t, kCutSourceCount> maxConsecutive{};
    uint64_t suppressedSyncs = 0;
    uint64_t roomLoads = 0;
    uint64_t roomUnloads = 0;
    uint64_t sceneChanges = 0;
    uint64_t teleportStamps = 0;
    uint64_t teleportDropped = 0;
};

void note_camera_cut(CutSource source) noexcept;
void note_camera_reset(const dCamera_c& camera) noexcept;
void latch_camera_cuts(int cameraId) noexcept;
void latch_teleports() noexcept;
void note_room_geometry(int roomNo, bool present) noexcept;
void note_teleport(const J3DModel* model) noexcept;

class ScopedCutSuppression {
public:
    [[nodiscard]] ScopedCutSuppression() noexcept;
    ~ScopedCutSuppression();
    ScopedCutSuppression(const ScopedCutSuppression&) = delete;
    ScopedCutSuppression& operator=(const ScopedCutSuppression&) = delete;
};

void begin_cut_frame() noexcept;
void apply_teleport_stamps() noexcept;
uint32_t advance_cut_epoch(const camera_process_class& camera) noexcept;
uint32_t cut_epoch() noexcept;
uint32_t advance_scene_epoch() noexcept;
uint32_t scene_epoch() noexcept;
uint32_t frames_since_scene_change() noexcept;

const CutStats& cut_stats() noexcept;
void reset_cut_stats() noexcept;
std::vector<std::string> describe_cuts();
void cuts_self_test(SelfTestReport& report);

}  // namespace dusk::render
