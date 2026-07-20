#include "presence.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lantern::tp {
namespace {

class Writer {
public:
    void u8(uint8_t value) { bytes.push_back(value); }
    void i8(int8_t value) { u8(static_cast<uint8_t>(value)); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value));
        u8(static_cast<uint8_t>(value >> 8));
    }
    void i16(int16_t value) { u16(static_cast<uint16_t>(value)); }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            u8(static_cast<uint8_t>(value >> shift));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    bool string(const std::string& value, size_t limit, std::string& error) {
        if (value.size() > limit || value.size() > 255) {
            error = "presence stage name exceeds bound";
            return false;
        }
        u8(static_cast<uint8_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
    std::vector<uint8_t> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> input) : bytes(input) {}
    bool u8(uint8_t& value) {
        if (offset >= bytes.size())
            return false;
        value = bytes[offset++];
        return true;
    }
    bool i8(int8_t& value) {
        uint8_t raw = 0;
        if (!u8(raw))
            return false;
        value = static_cast<int8_t>(raw);
        return true;
    }
    bool u16(uint16_t& value) {
        if (bytes.size() - offset < 2)
            return false;
        value =
            static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8);
        offset += 2;
        return true;
    }
    bool i16(int16_t& value) {
        uint16_t raw = 0;
        if (!u16(raw))
            return false;
        value = static_cast<int16_t>(raw);
        return true;
    }
    bool u32(uint32_t& value) {
        if (bytes.size() - offset < 4)
            return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
        offset += 4;
        return true;
    }
    bool i32(int32_t& value) {
        uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = static_cast<int32_t>(raw);
        return true;
    }
    bool string(std::string& value, size_t limit) {
        uint8_t size = 0;
        if (!u8(size) || size > limit || bytes.size() - offset < size)
            return false;
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return true;
    }
    bool done() const { return offset == bytes.size(); }

private:
    std::span<const uint8_t> bytes;
    size_t offset = 0;
};

int32_t quantize_position(float value) {
    constexpr double scale = 8.0;
    const double clamped = std::clamp(static_cast<double>(value) * scale,
        static_cast<double>(std::numeric_limits<int32_t>::min()),
        static_cast<double>(std::numeric_limits<int32_t>::max()));
    return static_cast<int32_t>(std::lround(clamped));
}

int16_t quantize_velocity(float value) {
    return static_cast<int16_t>(std::lround(std::clamp(value * 16.0f, -32768.0f, 32767.0f)));
}

int16_t quantize_basis(float value) {
    // J3D joint matrices carry accumulated animation/model scale, so basis elements are not
    // restricted to rotation's [-1, 1] range. Preserve values through almost +/-8 while retaining
    // substantially sub-pixel angular precision.
    return static_cast<int16_t>(std::lround(std::clamp(value * 4096.0f, -32768.0f, 32767.0f)));
}

int16_t quantize_translation(float value) {
    return static_cast<int16_t>(std::lround(std::clamp(value * 16.0f, -32768.0f, 32767.0f)));
}

float mix(float left, float right, float amount) {
    return left + (right - left) * amount;
}

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

struct AffineTransform {
    Vec3 translation;
    std::array<float, 9> residual;
    Quaternion rotation;
};

float dot(Vec3 left, Vec3 right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 cross(Vec3 left, Vec3 right) {
    return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

Vec3 scaled(Vec3 value, float amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

Vec3 subtract(Vec3 left, Vec3 right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

bool normalize(Vec3& value) {
    const float magnitude = std::sqrt(dot(value, value));
    if (!std::isfinite(magnitude) || magnitude < 0.00001f)
        return false;
    value = scaled(value, 1.0f / magnitude);
    return true;
}

Quaternion normalized(Quaternion value) {
    const float magnitude =
        std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (!std::isfinite(magnitude) || magnitude < 0.00001f)
        return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inverse = 1.0f / magnitude;
    return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
}

Quaternion quaternion_from_basis(Vec3 x_axis, Vec3 y_axis, Vec3 z_axis) {
    const float m00 = x_axis.x;
    const float m01 = y_axis.x;
    const float m02 = z_axis.x;
    const float m10 = x_axis.y;
    const float m11 = y_axis.y;
    const float m12 = z_axis.y;
    const float m20 = x_axis.z;
    const float m21 = y_axis.z;
    const float m22 = z_axis.z;
    Quaternion result{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float factor = 2.0f * std::sqrt(trace + 1.0f);
        result.w = 0.25f * factor;
        result.x = (m21 - m12) / factor;
        result.y = (m02 - m20) / factor;
        result.z = (m10 - m01) / factor;
    } else if (m00 > m11 && m00 > m22) {
        const float factor = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        result.w = (m21 - m12) / factor;
        result.x = 0.25f * factor;
        result.y = (m01 + m10) / factor;
        result.z = (m02 + m20) / factor;
    } else if (m11 > m22) {
        const float factor = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        result.w = (m02 - m20) / factor;
        result.x = (m01 + m10) / factor;
        result.y = 0.25f * factor;
        result.z = (m12 + m21) / factor;
    } else {
        const float factor = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        result.w = (m10 - m01) / factor;
        result.x = (m02 + m20) / factor;
        result.y = (m12 + m21) / factor;
        result.z = 0.25f * factor;
    }
    return normalized(result);
}

bool decompose_affine(const JointTransform& joint, AffineTransform& out) {
    Vec3 x_axis{joint.matrix[0], joint.matrix[4], joint.matrix[8]};
    Vec3 y_axis{joint.matrix[1], joint.matrix[5], joint.matrix[9]};
    const Vec3 source_z{joint.matrix[2], joint.matrix[6], joint.matrix[10]};
    const Vec3 source_x = x_axis;
    const Vec3 source_y = y_axis;
    if (!normalize(x_axis))
        return false;

    // Extract an orthonormal basis for quaternion interpolation, but retain the complete affine
    // residual Q^T*A. J3D matrices can contain accumulated non-uniform scale and shear; keeping
    // only three scale values changes the pose even when interpolating two identical snapshots.
    y_axis = subtract(y_axis, scaled(x_axis, dot(y_axis, x_axis)));
    if (!normalize(y_axis))
        return false;
    Vec3 z_axis = cross(x_axis, y_axis);
    if (!normalize(z_axis))
        return false;

    out.translation = {joint.matrix[3], joint.matrix[7], joint.matrix[11]};
    out.residual = {dot(x_axis, source_x), dot(x_axis, source_y), dot(x_axis, source_z),
        dot(y_axis, source_x), dot(y_axis, source_y), dot(y_axis, source_z), dot(z_axis, source_x),
        dot(z_axis, source_y), dot(z_axis, source_z)};
    if (!std::all_of(out.residual.begin(), out.residual.end(),
            [](float value) { return std::isfinite(value); }))
    {
        return false;
    }
    out.rotation = quaternion_from_basis(x_axis, y_axis, z_axis);
    return true;
}

Quaternion slerp(Quaternion left, Quaternion right, float amount) {
    float cosine = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    if (cosine < 0.0f) {
        right = {-right.x, -right.y, -right.z, -right.w};
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    if (cosine > 0.9995f) {
        return normalized({mix(left.x, right.x, amount), mix(left.y, right.y, amount),
            mix(left.z, right.z, amount), mix(left.w, right.w, amount)});
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    if (std::abs(sine) < 0.00001f)
        return left;
    const float left_weight = std::sin((1.0f - amount) * angle) / sine;
    const float right_weight = std::sin(amount * angle) / sine;
    return normalized({left.x * left_weight + right.x * right_weight,
        left.y * left_weight + right.y * right_weight,
        left.z * left_weight + right.z * right_weight,
        left.w * left_weight + right.w * right_weight});
}

JointTransform compose_affine(
    const AffineTransform& left, const AffineTransform& right, float amount) {
    const Quaternion rotation = slerp(left.rotation, right.rotation, amount);
    std::array<float, 9> residual;
    for (size_t index = 0; index < residual.size(); ++index)
        residual[index] = mix(left.residual[index], right.residual[index], amount);
    const float xx = rotation.x * rotation.x;
    const float yy = rotation.y * rotation.y;
    const float zz = rotation.z * rotation.z;
    const float xy = rotation.x * rotation.y;
    const float xz = rotation.x * rotation.z;
    const float yz = rotation.y * rotation.z;
    const float wx = rotation.w * rotation.x;
    const float wy = rotation.w * rotation.y;
    const float wz = rotation.w * rotation.z;
    const std::array<float, 9> rotation_matrix = {1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz),
        2.0f * (xz + wy), 2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx),
        2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)};

    JointTransform result;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            result.matrix[row * 4 + column] = rotation_matrix[row * 3] * residual[column] +
                                              rotation_matrix[row * 3 + 1] * residual[3 + column] +
                                              rotation_matrix[row * 3 + 2] * residual[6 + column];
        }
    }
    result.matrix[3] = mix(left.translation.x, right.translation.x, amount);
    result.matrix[7] = mix(left.translation.y, right.translation.y, amount);
    result.matrix[11] = mix(left.translation.z, right.translation.z, amount);
    return result;
}

JointTransform blend_joint(const JointTransform& left, const JointTransform& right, float amount) {
    AffineTransform left_affine;
    AffineTransform right_affine;
    if (!decompose_affine(left, left_affine) || !decompose_affine(right, right_affine))
        return amount < 0.5f ? left : right;
    return compose_affine(left_affine, right_affine, amount);
}

PoseSnapshot blend(const PoseSnapshot& left, const PoseSnapshot& right, float amount) {
    if (!same_area(left, right) || left.wolf != right.wolf ||
        left.human_model != right.human_model || left.joints.size() != right.joints.size())
    {
        return amount < 0.5f ? left : right;
    }
    PoseSnapshot result = right;
    for (size_t i = 0; i < 3; ++i) {
        result.position[i] = mix(left.position[i], right.position[i], amount);
        result.velocity[i] = mix(left.velocity[i], right.velocity[i], amount);
    }
    for (size_t joint = 0; joint < result.joints.size(); ++joint)
        result.joints[joint] = blend_joint(left.joints[joint], right.joints[joint], amount);
    return result;
}

}  // namespace

bool encode_pose(const PoseSnapshot& pose, std::vector<uint8_t>& out, std::string& error) {
    if (pose.joints.empty() || pose.joints.size() > kMaxPoseJoints || pose.stage.empty() ||
        pose.stage.size() > 16)
    {
        error = "invalid presence snapshot";
        return false;
    }
    Writer writer;
    writer.u8(kPresenceWireVersion);
    writer.u32(pose.sequence);
    writer.u32(pose.timestamp_ms);
    if (!writer.string(pose.stage, 16, error))
        return false;
    writer.i8(pose.layer);
    writer.i8(pose.room);
    writer.u8(pose.wolf ? 1 : 0);
    writer.u8(static_cast<uint8_t>(pose.human_model));
    writer.u8(static_cast<uint8_t>(pose.joints.size()));
    for (float component : pose.position)
        writer.i32(quantize_position(component));
    for (int16_t component : pose.rotation)
        writer.i16(component);
    for (float component : pose.velocity)
        writer.i16(quantize_velocity(component));
    writer.u32(pose.equipment);
    for (const auto& joint : pose.joints) {
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 3; ++column) {
                writer.i16(quantize_basis(joint.matrix[row * 4 + column]));
            }
            writer.i16(quantize_translation(joint.matrix[row * 4 + 3]));
        }
    }
    if (writer.bytes.size() > lantern::protocol::kMaxModulePayloadBytes) {
        error = "presence snapshot exceeds module payload bound";
        return false;
    }
    out = std::move(writer.bytes);
    return true;
}

bool decode_pose(std::span<const uint8_t> bytes, PoseSnapshot& out, std::string& error) {
    Reader reader(bytes);
    uint8_t version = 0;
    uint8_t wolf = 0;
    uint8_t human_model = 0;
    uint8_t joint_count = 0;
    if (!reader.u8(version) || version != kPresenceWireVersion || !reader.u32(out.sequence) ||
        !reader.u32(out.timestamp_ms) || !reader.string(out.stage, 16) || !reader.i8(out.layer) ||
        !reader.i8(out.room) || !reader.u8(wolf) || wolf > 1 || !reader.u8(human_model) ||
        human_model > static_cast<uint8_t>(HumanModelVariant::Magic) || !reader.u8(joint_count) ||
        joint_count == 0 || joint_count > kMaxPoseJoints)
    {
        error = "malformed presence header";
        return false;
    }
    out.wolf = wolf != 0;
    out.human_model = static_cast<HumanModelVariant>(human_model);
    for (float& component : out.position) {
        int32_t value = 0;
        if (!reader.i32(value))
            return error = "truncated position", false;
        component = static_cast<float>(value) / 8.0f;
    }
    for (int16_t& component : out.rotation) {
        if (!reader.i16(component))
            return error = "truncated rotation", false;
    }
    for (float& component : out.velocity) {
        int16_t value = 0;
        if (!reader.i16(value))
            return error = "truncated velocity", false;
        component = static_cast<float>(value) / 16.0f;
    }
    if (!reader.u32(out.equipment))
        return error = "truncated equipment", false;
    out.joints.clear();
    out.joints.resize(joint_count);
    for (auto& joint : out.joints) {
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 3; ++column) {
                int16_t value = 0;
                if (!reader.i16(value))
                    return error = "truncated joint basis", false;
                joint.matrix[row * 4 + column] = static_cast<float>(value) / 4096.0f;
            }
            int16_t translation = 0;
            if (!reader.i16(translation))
                return error = "truncated joint translation", false;
            joint.matrix[row * 4 + 3] = static_cast<float>(translation) / 16.0f;
        }
    }
    if (!reader.done())
        return error = "trailing presence bytes", false;
    return true;
}

bool same_area(const PoseSnapshot& left, const PoseSnapshot& right) {
    return left.stage == right.stage && left.layer == right.layer && left.room == right.room;
}

bool is_teleport(const PoseSnapshot& previous, const PoseSnapshot& next) {
    if (!same_area(previous, next) || previous.wolf != next.wolf ||
        previous.human_model != next.human_model)
        return true;
    float distance_squared = 0.0f;
    for (size_t i = 0; i < 3; ++i) {
        const float delta = next.position[i] - previous.position[i];
        distance_squared += delta * delta;
    }
    return distance_squared > 800.0f * 800.0f;
}

bool interpolate_pose(const std::deque<TimedPose>& poses, std::chrono::steady_clock::time_point now,
    PoseSnapshot& out) {
    if (poses.empty() || now - poses.back().received_at > std::chrono::seconds(2))
        return false;
    const auto target = now - std::chrono::milliseconds(100);
    if (target <= poses.front().received_at) {
        out = poses.front().pose;
        return true;
    }
    for (size_t i = 1; i < poses.size(); ++i) {
        if (poses[i].received_at >= target) {
            const auto interval = poses[i].received_at - poses[i - 1].received_at;
            if (interval <= std::chrono::steady_clock::duration::zero()) {
                out = poses[i].pose;
                return true;
            }
            const float amount =
                std::chrono::duration<float>(target - poses[i - 1].received_at).count() /
                std::chrono::duration<float>(interval).count();
            out = blend(poses[i - 1].pose, poses[i].pose, std::clamp(amount, 0.0f, 1.0f));
            return true;
        }
    }
    out = poses.back().pose;
    const auto elapsed = std::min(now - poses.back().received_at,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(250)));
    const float seconds = std::chrono::duration<float>(elapsed).count();
    for (size_t i = 0; i < 3; ++i)
        out.position[i] += out.velocity[i] * seconds;
    return true;
}

}  // namespace lantern::tp
