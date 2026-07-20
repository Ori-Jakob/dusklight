#include "network_client.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using lantern::net::ClientEvent;
using lantern::net::ClientEventType;
using lantern::net::NetworkClient;
using lantern::protocol::FrameView;
using lantern::protocol::MessageType;

int failures = 0;

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression << '\n';    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

lantern::protocol::ModuleManifest presence_module() {
    return {
        .module_id = "dev.twilitrealm.lantern.tp.presence",
        .owner_mod_id = "dev.twilitrealm.lantern.tp",
        .owner_mod_version = "0.1.5",
        .display_version = "0.1.5",
        .protocol_major = 2,
        .protocol_minor = 0,
        .minimum_peer_minor = 0,
        .flags = lantern::protocol::ModuleRequired,
        .distribution_id = "dev.twilitrealm.lantern.tp",
    };
}

std::vector<uint8_t> hello_frame(
    std::string name, bool create, const std::string& room, bool include_presence = true) {
    lantern::protocol::Hello hello;
    hello.create_room = create;
    hello.room_code = room;
    hello.peer.display_name = std::move(name);
    hello.peer.color_rgb = 0x77C8FF;
    if (include_presence)
        hello.peer.modules.push_back(presence_module());
    std::vector<uint8_t> frame;
    std::string error;
    if (!lantern::protocol::encode(hello, frame, error)) {
        std::cerr << "Could not encode hello: " << error << '\n';
    }
    return frame;
}

bool wait_for_frame(NetworkClient& client, MessageType wanted, FrameView& out,
    std::vector<uint8_t>& storage, std::chrono::seconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ClientEvent event;
        while (client.poll(event)) {
            if (event.type == ClientEventType::Error) {
                std::cerr << "Network error: " << event.detail << '\n';
            }
            if (event.type != ClientEventType::Frame)
                continue;
            FrameView decoded;
            std::string error;
            if (!lantern::protocol::decode_frame(event.frame, decoded, error))
                continue;
            if (decoded.type == wanted) {
                storage = std::move(event.frame);
                return lantern::protocol::decode_frame(storage, out, error);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: lantern_relay_integration PORT\n";
        return EXIT_FAILURE;
    }
    const std::string address = std::string("127.0.0.1:") + argv[1];
    std::string error;
    NetworkClient first;
    NetworkClient second;
    NetworkClient incompatible;
    CHECK(first.start(error));
    CHECK(second.start(error));
    CHECK(incompatible.start(error));

    first.connect(address, hello_frame("Link", true, ""));
    FrameView frame;
    std::vector<uint8_t> storage;
    CHECK(wait_for_frame(first, MessageType::Welcome, frame, storage));
    lantern::protocol::Welcome first_welcome;
    CHECK(lantern::protocol::decode(frame, first_welcome, error));
    CHECK(first_welcome.self_peer_id != 0);
    CHECK(!first_welcome.room_code.empty());

    second.connect(address, hello_frame("Midna", false, first_welcome.room_code));
    CHECK(wait_for_frame(second, MessageType::Welcome, frame, storage));
    lantern::protocol::Welcome second_welcome;
    CHECK(lantern::protocol::decode(frame, second_welcome, error));
    CHECK(second_welcome.self_peer_id != 0);
    CHECK(second_welcome.self_peer_id != first_welcome.self_peer_id);
    CHECK(wait_for_frame(first, MessageType::PeerJoined, frame, storage));

    lantern::protocol::ModuleData module_message;
    module_message.sender_peer_id = 0xDEADBEEF;  // The relay must replace this value.
    module_message.delivery = lantern::protocol::Delivery::ReliableOrdered;
    module_message.sequence = 7;
    module_message.module_id = presence_module().module_id;
    module_message.payload = {1, 2, 3, 4};
    std::vector<uint8_t> module_frame;
    CHECK(lantern::protocol::encode(module_message, module_frame, error));
    CHECK(second.send(std::move(module_frame), true));
    CHECK(wait_for_frame(first, MessageType::ModuleData, frame, storage));
    lantern::protocol::ModuleData received;
    CHECK(lantern::protocol::decode(frame, received, error));
    CHECK(received.sender_peer_id == second_welcome.self_peer_id);
    CHECK(received.sender_peer_id != 0xDEADBEEF);

    incompatible.connect(
        address, hello_frame("Cosmetic only", false, first_welcome.room_code, false));
    CHECK(wait_for_frame(incompatible, MessageType::Reject, frame, storage));
    lantern::protocol::Reject rejection;
    CHECK(lantern::protocol::decode(frame, rejection, error));
    CHECK(rejection.reason.find("required module") != std::string::npos);

    lantern::protocol::ManifestUpdate update;
    std::vector<uint8_t> update_frame;
    CHECK(lantern::protocol::encode(update, update_frame, error));
    CHECK(second.send(std::move(update_frame), true));
    CHECK(wait_for_frame(second, MessageType::Reject, frame, storage));
    CHECK(lantern::protocol::decode(frame, rejection, error));
    CHECK(rejection.reason.find("no longer room-compatible") != std::string::npos);

    // Bring the room to the eight-peer target and exercise presence-shaped unreliable traffic at
    // 30 Hz. The test deliberately only requires fresh traffic to arrive: UDP delivery is not
    // guaranteed and the client queue is latest-wins by design.
    std::vector<std::unique_ptr<NetworkClient>> stress_clients;
    for (int index = 0; index < 7; ++index) {
        auto client = std::make_unique<NetworkClient>();
        CHECK(client->start(error));
        client->connect(address,
            hello_frame("Stress " + std::to_string(index), false, first_welcome.room_code));
        CHECK(wait_for_frame(*client, MessageType::Welcome, frame, storage));
        CHECK(wait_for_frame(first, MessageType::PeerJoined, frame, storage));
        stress_clients.push_back(std::move(client));
    }
    for (uint32_t tick = 0; tick < 15; ++tick) {
        for (auto& client : stress_clients) {
            lantern::protocol::ModuleData pose;
            pose.delivery = lantern::protocol::Delivery::UnreliableLatest;
            pose.sequence = tick + 1;
            pose.module_id = presence_module().module_id;
            pose.payload = {static_cast<uint8_t>(tick), 2, 3, 4};
            std::vector<uint8_t> encoded;
            CHECK(lantern::protocol::encode(pose, encoded, error));
            CHECK(client->send(std::move(encoded), false, "presence"));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(33333));
    }
    size_t received_stress_poses = 0;
    const auto stress_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < stress_deadline && received_stress_poses < 7) {
        ClientEvent event;
        while (first.poll(event)) {
            if (event.type != ClientEventType::Frame)
                continue;
            FrameView decoded;
            if (lantern::protocol::decode_frame(event.frame, decoded, error) &&
                decoded.type == MessageType::ModuleData)
                ++received_stress_poses;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(received_stress_poses >= 7);

    // The 65th reliable module message within one rate window must close the sender.
    for (uint32_t sequence = 1; sequence <= 65; ++sequence) {
        lantern::protocol::ModuleData flood;
        flood.delivery = lantern::protocol::Delivery::ReliableOrdered;
        flood.sequence = sequence;
        flood.module_id = presence_module().module_id;
        flood.payload = {1};
        std::vector<uint8_t> encoded;
        CHECK(lantern::protocol::encode(flood, encoded, error));
        CHECK(first.send(std::move(encoded), true));
    }
    CHECK(wait_for_frame(first, MessageType::Reject, frame, storage));
    CHECK(lantern::protocol::decode(frame, rejection, error));
    CHECK(rejection.reason.find("rate limit") != std::string::npos);

    for (auto& client : stress_clients)
        client->stop();
    incompatible.stop();
    second.stop();
    first.stop();
    if (failures != 0) {
        std::cerr << failures << " relay integration test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Lantern relay integration tests passed\n";
    return EXIT_SUCCESS;
}
