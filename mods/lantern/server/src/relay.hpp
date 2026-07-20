#pragma once

#include "gns_runtime.hpp"
#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <steam/steamnetworkingtypes.h>

class ISteamNetworkingSockets;
struct SteamNetConnectionStatusChangedCallback_t;

namespace lantern::server {

class Relay {
public:
    Relay() = default;
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;
    ~Relay();

    bool start(uint16_t port, std::string& error);
    void run();
    void request_stop();
    void stop();

private:
    struct RateWindow {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        uint32_t messages = 0;
        size_t bytes = 0;
    };
    struct Client {
        HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
        bool joined = false;
        std::string room_code;
        protocol::PeerManifest peer;
        std::chrono::steady_clock::time_point connected_at = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_ping = std::chrono::steady_clock::now();
        RateWindow reliable_rate;
        RateWindow unreliable_rate;
    };
    struct Room {
        std::vector<HSteamNetConnection> clients;
    };

    static Relay* callback_instance_;
    static void connection_callback(SteamNetConnectionStatusChangedCallback_t* info);
    void connection_changed(SteamNetConnectionStatusChangedCallback_t* info);
    void poll_messages();
    void process_message(Client& client, std::span<const uint8_t> bytes);
    void process_hello(Client& client, const protocol::FrameView& frame);
    void process_manifest_update(Client& client, const protocol::FrameView& frame);
    void process_module_data(Client& client, const protocol::FrameView& frame);
    void maintain_clients();

    bool join_room(Client& client, protocol::Hello hello, std::string& error);
    std::string make_room_code();
    void reject_and_drop(HSteamNetConnection connection, const std::string& reason);
    void drop_client(HSteamNetConnection connection, const std::string& reason, bool close_socket);
    bool send_frame(
        HSteamNetConnection connection, const std::vector<uint8_t>& frame, bool reliable);
    void broadcast(const Room& room, const std::vector<uint8_t>& frame,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid, bool reliable = true);
    bool within_rate(Client& client, bool reliable, size_t bytes);
    bool recipient_supports(
        const Client& recipient, const protocol::ModuleManifest& sender_module) const;

    net::GnsRuntime runtime_;
    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
    std::atomic<bool> running_{false};
    std::unordered_map<HSteamNetConnection, Client> clients_;
    std::unordered_map<std::string, Room> rooms_;
    std::mt19937_64 random_{std::random_device{}()};
    uint64_t next_peer_id_ = 1;
};

}  // namespace lantern::server
