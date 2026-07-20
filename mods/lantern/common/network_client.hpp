#pragma once

#include "gns_runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct SteamNetConnectionStatusChangedCallback_t;
typedef uint32_t HSteamNetConnection;

namespace lantern::net {

enum class ClientEventType {
    TransportConnected,
    TransportDisconnected,
    Frame,
    Error,
};

struct ClientEvent {
    ClientEventType type = ClientEventType::Error;
    std::string detail;
    std::vector<uint8_t> frame;
};

class NetworkClient {
public:
    NetworkClient();
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;
    ~NetworkClient();

    bool start(std::string& error);
    void stop();
    void connect(std::string address, std::vector<uint8_t> hello_frame);
    void disconnect(std::string reason = "client disconnect");
    bool send(std::vector<uint8_t> frame, bool reliable, std::string latest_key = {});
    bool poll(ClientEvent& out);

private:
    enum class CommandType { Connect, Disconnect };
    struct Command {
        CommandType type{};
        std::string text;
        std::vector<uint8_t> frame;
    };
    struct Outbound {
        bool reliable = true;
        std::string latest_key;
        std::vector<uint8_t> frame;
    };

    static void connection_changed(SteamNetConnectionStatusChangedCallback_t* info);
    void on_connection_changed(SteamNetConnectionStatusChangedCallback_t* info);
    void run();
    void begin_connect(const Command& command);
    void close_connection(const char* reason, bool linger);
    void push_event(ClientEvent event);
    void receive_frames();
    void flush_outbound();

    GnsRuntime runtime_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    std::deque<Outbound> outbound_;
    size_t reliable_queued_bytes_ = 0;
    std::deque<ClientEvent> inbound_;
    std::atomic<HSteamNetConnection> connection_{0};
    std::vector<uint8_t> pending_hello_;
};

}  // namespace lantern::net
