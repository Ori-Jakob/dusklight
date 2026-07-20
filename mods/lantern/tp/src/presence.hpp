#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace lantern::tp {

inline constexpr uint8_t kPresenceWireVersion = 2;
inline constexpr size_t kMaxPoseJoints = 40;

enum class HumanModelVariant : uint8_t {
    Hero = 0,
    Ordon = 1,
    Zora = 2,
    Magic = 3,
};

struct JointTransform {
    // Row-major 3x4 affine matrix. Joint zero is actor-relative root; later joints are
    // root-relative.
    std::array<float, 12> matrix{};
};

struct PoseSnapshot {
    uint32_t sequence = 0;
    uint32_t timestamp_ms = 0;
    std::string stage;
    int8_t layer = -1;
    int8_t room = -1;
    bool wolf = false;
    HumanModelVariant human_model = HumanModelVariant::Hero;
    std::array<float, 3> position{};
    std::array<int16_t, 3> rotation{};
    std::array<float, 3> velocity{};
    uint32_t equipment = 0;
    std::vector<JointTransform> joints;
};

struct TimedPose {
    PoseSnapshot pose;
    std::chrono::steady_clock::time_point received_at;
};

bool encode_pose(const PoseSnapshot& pose, std::vector<uint8_t>& out, std::string& error);
bool decode_pose(std::span<const uint8_t> bytes, PoseSnapshot& out, std::string& error);
bool same_area(const PoseSnapshot& left, const PoseSnapshot& right);
bool is_teleport(const PoseSnapshot& previous, const PoseSnapshot& next);
bool interpolate_pose(const std::deque<TimedPose>& poses, std::chrono::steady_clock::time_point now,
    PoseSnapshot& out);

}  // namespace lantern::tp
