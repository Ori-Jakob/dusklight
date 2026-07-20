#include "loopback_transport.hpp"
#include "presence.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression << '\n';    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

lantern::protocol::ModuleManifest module(std::string id, uint32_t flags = 0) {
    return {
        .module_id = std::move(id),
        .owner_mod_id = "dev.test.mod",
        .owner_mod_version = "1.0.0",
        .display_version = "1.0.0",
        .protocol_major = 1,
        .protocol_minor = 2,
        .minimum_peer_minor = 0,
        .flags = flags,
    };
}

void protocol_round_trip() {
    lantern::protocol::Hello hello;
    hello.create_room = true;
    hello.peer.display_name = "Midna";
    hello.peer.color_rgb = 0x8844CC;
    hello.peer.modules.push_back(module("dev.test.presence", lantern::protocol::ModuleRequired));
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(lantern::protocol::encode(hello, bytes, error));
    lantern::protocol::FrameView frame;
    CHECK(lantern::protocol::decode_frame(bytes, frame, error));
    lantern::protocol::Hello decoded;
    CHECK(lantern::protocol::decode(frame, decoded, error));
    CHECK(decoded.create_room);
    CHECK(decoded.peer.display_name == "Midna");
    CHECK(decoded.peer.modules.size() == 1);
}

void malformed_and_bounds() {
    std::string error;
    lantern::protocol::FrameView frame;
    CHECK(!lantern::protocol::decode_frame(std::span<const uint8_t>{}, frame, error));
    auto bad = module(std::string(lantern::protocol::kMaxIdBytes + 1, 'x'));
    CHECK(!lantern::protocol::validate_manifest({bad}, error));
    auto duplicate = module("same");
    CHECK(!lantern::protocol::validate_manifest({duplicate, duplicate}, error));

    lantern::protocol::Hello hello;
    hello.create_room = true;
    hello.peer.display_name = "Link";
    hello.peer.modules.push_back(module("valid"));
    std::vector<uint8_t> encoded;
    CHECK(lantern::protocol::encode(hello, encoded, error));
    encoded.pop_back();
    CHECK(!lantern::protocol::decode_frame(encoded, frame, error));
}

void compatibility_matrix() {
    auto required = module("presence", lantern::protocol::ModuleRequired);
    auto exact = required;
    CHECK(lantern::protocol::compare_manifests({required}, {exact}).can_join);
    CHECK(!lantern::protocol::compare_manifests({required}, {}).can_join);

    auto newer = exact;
    newer.protocol_minor = 4;
    newer.minimum_peer_minor = 2;
    CHECK(lantern::protocol::compare_manifests({required}, {newer}).can_join);
    newer.minimum_peer_minor = 3;
    CHECK(!lantern::protocol::compare_manifests({required}, {newer}).can_join);

    auto cosmetic = module("reshade", lantern::protocol::ModuleCosmetic);
    CHECK(lantern::protocol::compare_manifests({required, cosmetic}, {exact}).can_join);

    auto tagged = required;
    tagged.compatibility_tag = "seed-a";
    auto other_seed = tagged;
    other_seed.compatibility_tag = "seed-b";
    CHECK(!lantern::protocol::compare_manifests({tagged}, {other_seed}).can_join);

    auto optional_left = module("optional");
    auto optional_right = optional_left;
    optional_right.protocol_major = 2;
    const auto optional_report =
        lantern::protocol::compare_manifests({optional_left}, {optional_right});
    CHECK(optional_report.can_join);
    CHECK(optional_report.compatible_modules.empty());
}

void sequence_wraparound() {
    CHECK(lantern::protocol::sequence_newer(11, 10));
    CHECK(!lantern::protocol::sequence_newer(10, 10));
    CHECK(lantern::protocol::sequence_newer(0, std::numeric_limits<uint32_t>::max()));
    CHECK(!lantern::protocol::sequence_newer(std::numeric_limits<uint32_t>::max(), 0));
}

lantern::tp::PoseSnapshot pose(float x, uint32_t sequence = 1) {
    lantern::tp::PoseSnapshot result;
    result.sequence = sequence;
    result.timestamp_ms = sequence * 50;
    result.stage = "F_SP108";
    result.layer = 0;
    result.room = 1;
    result.human_model = lantern::tp::HumanModelVariant::Magic;
    result.position = {x, 20.0f, 30.0f};
    result.velocity = {10.0f, 0.0f, 0.0f};
    lantern::tp::JointTransform joint;
    joint.matrix = {1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1, 0};
    result.joints.push_back(joint);
    return result;
}

void pose_codec_and_interpolation() {
    auto source = pose(10.125f);
    source.joints[0].matrix[0] = 1.5f;
    source.joints[0].matrix[5] = 0.75f;
    source.joints[0].matrix[10] = 1.25f;
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(lantern::tp::encode_pose(source, bytes, error));
    lantern::tp::PoseSnapshot decoded;
    CHECK(lantern::tp::decode_pose(bytes, decoded, error));
    CHECK(decoded.stage == source.stage);
    CHECK(decoded.human_model == source.human_model);
    CHECK(decoded.position[0] == source.position[0]);
    CHECK(decoded.joints.size() == 1);
    CHECK(std::abs(decoded.joints[0].matrix[0] - 1.5f) < 0.0003f);
    CHECK(std::abs(decoded.joints[0].matrix[5] - 0.75f) < 0.0003f);
    CHECK(std::abs(decoded.joints[0].matrix[10] - 1.25f) < 0.0003f);

    const auto start = std::chrono::steady_clock::now();
    std::deque<lantern::tp::TimedPose> sheared_poses;
    auto sheared_pose = pose(0, 1);
    sheared_pose.joints[0].matrix = {
        1.5f, 0.3f, -0.2f, 4.0f, 0.0f, 0.75f, 0.25f, 2.0f, 0.0f, 0.0f, -1.25f, -3.0f};
    sheared_poses.push_back({sheared_pose, start});
    sheared_poses.push_back({sheared_pose, start + std::chrono::milliseconds(100)});
    lantern::tp::PoseSnapshot sheared_sample;
    CHECK(lantern::tp::interpolate_pose(
        sheared_poses, start + std::chrono::milliseconds(150), sheared_sample));
    for (size_t component = 0; component < sheared_pose.joints[0].matrix.size(); ++component) {
        CHECK(std::abs(sheared_sample.joints[0].matrix[component] -
                       sheared_pose.joints[0].matrix[component]) < 0.0001f);
    }

    std::deque<lantern::tp::TimedPose> poses;
    auto left_pose = pose(0, 1);
    auto right_pose = pose(10, 2);
    // Opposing 90-degree rotations collapse to a singular basis when matrix elements are lerped
    // independently. Rotation interpolation must keep the skinned joint rigid.
    left_pose.joints[0].matrix = {0, 0, 1, 0, 0, 1, 0, 2, -1, 0, 0, 0};
    right_pose.joints[0].matrix = {0, 0, -1, 0, 0, 1, 0, 2, 1, 0, 0, 0};
    poses.push_back({left_pose, start});
    poses.push_back({right_pose, start + std::chrono::milliseconds(100)});
    lantern::tp::PoseSnapshot sampled;
    CHECK(lantern::tp::interpolate_pose(poses, start + std::chrono::milliseconds(150), sampled));
    CHECK(sampled.position[0] > 4.9f && sampled.position[0] < 5.1f);
    const auto& matrix = sampled.joints[0].matrix;
    const float determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
                              matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
                              matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    CHECK(determinant > 0.99f && determinant < 1.01f);
    CHECK(lantern::tp::is_teleport(pose(0), pose(1000)));
}

void loopback_latest_wins() {
    lantern::test::LoopbackLink link;
    CHECK(link.first().send({1}, lantern::protocol::Delivery::ReliableOrdered));
    CHECK(link.first().send({2}, lantern::protocol::Delivery::UnreliableLatest, "pose"));
    CHECK(link.first().send({3}, lantern::protocol::Delivery::UnreliableLatest, "pose"));
    std::vector<uint8_t> received;
    CHECK(link.second().receive(received) && received[0] == 1);
    CHECK(link.second().receive(received) && received[0] == 3);
    CHECK(!link.second().receive(received));
    link.drop_next_unreliable();
    CHECK(link.first().send({4}, lantern::protocol::Delivery::UnreliableLatest, "pose"));
    CHECK(!link.second().receive(received));
}

}  // namespace

int main() {
    protocol_round_trip();
    malformed_and_bounds();
    compatibility_matrix();
    sequence_wraparound();
    pose_codec_and_interpolation();
    loopback_latest_wins();
    if (failures != 0) {
        std::cerr << failures << " Lantern test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Lantern protocol tests passed\n";
    return EXIT_SUCCESS;
}
