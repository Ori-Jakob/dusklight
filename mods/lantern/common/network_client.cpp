#include "network_client.hpp"

#include "protocol.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace lantern::net {
namespace {

constexpr size_t kMaxInboundEvents = 1024;
constexpr size_t kMaxReliableFrames = 256;
constexpr size_t kMaxReliableBytes = 4 * 1024 * 1024;

}  // namespace

NetworkClient::NetworkClient() = default;

NetworkClient::~NetworkClient() {
    stop();
}

bool NetworkClient::start(std::string& error) {
    if (running_) {
        return true;
    }
    if (!runtime_.initialize(error)) {
        return false;
    }
    running_ = true;
    thread_ = std::thread(&NetworkClient::run, this);
    return true;
}

void NetworkClient::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    runtime_.shutdown();
    std::lock_guard lock(mutex_);
    commands_.clear();
    outbound_.clear();
    inbound_.clear();
    reliable_queued_bytes_ = 0;
}

void NetworkClient::connect(std::string address, std::vector<uint8_t> hello_frame) {
    std::lock_guard lock(mutex_);
    commands_.push_back({CommandType::Connect, std::move(address), std::move(hello_frame)});
    wake_.notify_one();
}

void NetworkClient::disconnect(std::string reason) {
    std::lock_guard lock(mutex_);
    commands_.push_back({CommandType::Disconnect, std::move(reason), {}});
    wake_.notify_one();
}

bool NetworkClient::send(std::vector<uint8_t> frame, bool reliable, std::string latest_key) {
    if (frame.empty() || frame.size() > protocol::kMaxFrameBytes || !running_) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (reliable) {
        const size_t reliable_count = static_cast<size_t>(std::count_if(outbound_.begin(),
            outbound_.end(), [](const Outbound& queued) { return queued.reliable; }));
        if (reliable_count >= kMaxReliableFrames ||
            reliable_queued_bytes_ + frame.size() > kMaxReliableBytes)
        {
            return false;
        }
        reliable_queued_bytes_ += frame.size();
    } else if (!latest_key.empty()) {
        const auto existing =
            std::find_if(outbound_.begin(), outbound_.end(), [&](const Outbound& queued) {
                return !queued.reliable && queued.latest_key == latest_key;
            });
        if (existing != outbound_.end()) {
            existing->frame = std::move(frame);
            return true;
        }
    }
    outbound_.push_back({reliable, std::move(latest_key), std::move(frame)});
    wake_.notify_one();
    return true;
}

bool NetworkClient::poll(ClientEvent& out) {
    std::lock_guard lock(mutex_);
    if (inbound_.empty()) {
        return false;
    }
    out = std::move(inbound_.front());
    inbound_.pop_front();
    return true;
}

void NetworkClient::connection_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    auto* instance = reinterpret_cast<NetworkClient*>(info->m_info.m_nUserData);
    if (instance != nullptr) {
        instance->on_connection_changed(info);
    }
}

void NetworkClient::on_connection_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    if (info->m_hConn != connection_.load()) {
        return;
    }
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connected: {
        push_event({ClientEventType::TransportConnected, {}, {}});
        std::vector<uint8_t> hello;
        {
            std::lock_guard lock(mutex_);
            hello.swap(pending_hello_);
        }
        if (!hello.empty()) {
            SteamNetworkingSockets()->SendMessageToConnection(connection_.load(), hello.data(),
                static_cast<uint32_t>(hello.size()), k_nSteamNetworkingSend_Reliable, nullptr);
        }
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        std::string detail = info->m_info.m_szEndDebug;
        close_connection("connection ended", false);
        push_event({ClientEventType::TransportDisconnected, std::move(detail), {}});
        break;
    }
    default:
        break;
    }
}

void NetworkClient::run() {
    while (running_) {
        std::deque<Command> commands;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(2),
                [&] { return !running_ || !commands_.empty() || !outbound_.empty(); });
            commands.swap(commands_);
        }
        for (const auto& command : commands) {
            if (command.type == CommandType::Connect) {
                begin_connect(command);
            } else {
                close_connection(command.text.c_str(), true);
                push_event({ClientEventType::TransportDisconnected, command.text, {}});
            }
        }
        SteamNetworkingSockets()->RunCallbacks();
        receive_frames();
        flush_outbound();
    }
    close_connection("Lantern shutting down", false);
}

void NetworkClient::begin_connect(const Command& command) {
    close_connection("new connection", false);
    SteamNetworkingIPAddr address{};
    if (!address.ParseString(command.text.c_str())) {
        push_event({ClientEventType::Error, "Invalid server address: " + command.text, {}});
        return;
    }
    SteamNetworkingConfigValue_t options[2];
    options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(&NetworkClient::connection_changed));
    options[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
        static_cast<int64_t>(reinterpret_cast<intptr_t>(this)));
    {
        std::lock_guard lock(mutex_);
        pending_hello_ = command.frame;
    }
    connection_ = SteamNetworkingSockets()->ConnectByIPAddress(address, 2, options);
    if (connection_.load() == k_HSteamNetConnection_Invalid) {
        {
            std::lock_guard lock(mutex_);
            pending_hello_.clear();
        }
        push_event(
            {ClientEventType::Error, "GameNetworkingSockets could not start connection", {}});
    }
}

void NetworkClient::close_connection(const char* reason, bool linger) {
    const HSteamNetConnection connection = connection_.exchange(k_HSteamNetConnection_Invalid);
    if (connection != k_HSteamNetConnection_Invalid) {
        SteamNetworkingSockets()->CloseConnection(connection, 0, reason, linger);
    }
    std::lock_guard lock(mutex_);
    pending_hello_.clear();
    outbound_.clear();
    reliable_queued_bytes_ = 0;
}

void NetworkClient::push_event(ClientEvent event) {
    std::lock_guard lock(mutex_);
    if (inbound_.size() >= kMaxInboundEvents) {
        const auto discard = std::find_if(inbound_.begin(), inbound_.end(),
            [](const ClientEvent& queued) { return queued.type == ClientEventType::Frame; });
        if (discard != inbound_.end()) {
            inbound_.erase(discard);
        } else {
            inbound_.pop_front();
        }
    }
    inbound_.push_back(std::move(event));
}

void NetworkClient::receive_frames() {
    const HSteamNetConnection connection = connection_.load();
    if (connection == k_HSteamNetConnection_Invalid) {
        return;
    }
    for (;;) {
        SteamNetworkingMessage_t* message = nullptr;
        const int count =
            SteamNetworkingSockets()->ReceiveMessagesOnConnection(connection, &message, 1);
        if (count <= 0) {
            break;
        }
        ClientEvent event{ClientEventType::Frame, {}, {}};
        const auto* begin = static_cast<const uint8_t*>(message->m_pData);
        event.frame.assign(begin, begin + message->m_cbSize);
        message->Release();
        push_event(std::move(event));
    }
}

void NetworkClient::flush_outbound() {
    const HSteamNetConnection connection = connection_.load();
    if (connection == k_HSteamNetConnection_Invalid) {
        return;
    }
    std::deque<Outbound> outbound;
    {
        std::lock_guard lock(mutex_);
        outbound.swap(outbound_);
        reliable_queued_bytes_ = 0;
    }
    for (const auto& message : outbound) {
        const int flags = message.reliable ? k_nSteamNetworkingSend_Reliable :
                                             k_nSteamNetworkingSend_UnreliableNoNagle |
                                                 k_nSteamNetworkingSend_NoDelay;
        const EResult result = SteamNetworkingSockets()->SendMessageToConnection(connection,
            message.frame.data(), static_cast<uint32_t>(message.frame.size()), flags, nullptr);
        if (result != k_EResultOK && message.reliable) {
            push_event({ClientEventType::Error, "Reliable send queue rejected a message", {}});
        }
    }
}

}  // namespace lantern::net
