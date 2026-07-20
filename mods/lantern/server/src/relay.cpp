#include "relay.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>

namespace lantern::server {
namespace {

constexpr auto kHandshakeTimeout = std::chrono::seconds(10);
constexpr auto kHeartbeatInterval = std::chrono::seconds(5);
constexpr auto kHeartbeatTimeout = std::chrono::seconds(20);
constexpr size_t kMaxPendingReliableBytes = 1024 * 1024;
constexpr uint32_t kReliableMessagesPerSecond = 64;
constexpr size_t kReliableBytesPerSecond = 256 * 1024;
constexpr uint32_t kUnreliableMessagesPerSecond = 160;
constexpr size_t kUnreliableBytesPerSecond = 768 * 1024;

const protocol::ModuleManifest* find_module(
    const protocol::PeerManifest& peer, std::string_view module_id) {
    const auto it = std::find_if(peer.modules.begin(), peer.modules.end(),
        [&](const auto& module) { return module.module_id == module_id; });
    return it == peer.modules.end() ? nullptr : &*it;
}

}  // namespace

Relay* Relay::callback_instance_ = nullptr;

Relay::~Relay() {
    stop();
}

bool Relay::start(uint16_t port, std::string& error) {
    if (running_)
        return true;
    if (!runtime_.initialize(error))
        return false;
    sockets_ = SteamNetworkingSockets();
    callback_instance_ = this;

    SteamNetworkingIPAddr address{};
    address.Clear();
    address.m_port = port;
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(&Relay::connection_callback));
    listen_socket_ = sockets_->CreateListenSocketIP(address, 1, &option);
    if (listen_socket_ == k_HSteamListenSocket_Invalid) {
        error = "could not create UDP listen socket";
        stop();
        return false;
    }
    poll_group_ = sockets_->CreatePollGroup();
    if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
        error = "could not create connection poll group";
        stop();
        return false;
    }
    next_peer_id_ = std::max<uint64_t>(1, random_());
    running_ = true;
    std::cout << "Lantern relay listening on UDP port " << port << '\n';
    return true;
}

void Relay::run() {
    while (running_) {
        sockets_->RunCallbacks();
        poll_messages();
        maintain_clients();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void Relay::request_stop() {
    running_ = false;
}

void Relay::stop() {
    running_ = false;
    if (sockets_ != nullptr) {
        std::vector<HSteamNetConnection> connections;
        for (const auto& [connection, client] : clients_) {
            (void)client;
            connections.push_back(connection);
        }
        for (const auto connection : connections) {
            drop_client(connection, "relay shutdown", true);
        }
        if (listen_socket_ != k_HSteamListenSocket_Invalid) {
            sockets_->CloseListenSocket(listen_socket_);
            listen_socket_ = k_HSteamListenSocket_Invalid;
        }
        if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
            sockets_->DestroyPollGroup(poll_group_);
            poll_group_ = k_HSteamNetPollGroup_Invalid;
        }
    }
    sockets_ = nullptr;
    callback_instance_ = nullptr;
    rooms_.clear();
    clients_.clear();
    runtime_.shutdown();
}

void Relay::connection_callback(SteamNetConnectionStatusChangedCallback_t* info) {
    if (callback_instance_ != nullptr)
        callback_instance_->connection_changed(info);
}

void Relay::connection_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
        if (clients_.size() >= 256 || sockets_->AcceptConnection(info->m_hConn) != k_EResultOK ||
            !sockets_->SetConnectionPollGroup(info->m_hConn, poll_group_))
        {
            sockets_->CloseConnection(info->m_hConn, 1, "relay busy", false);
            return;
        }
        Client client;
        client.connection = info->m_hConn;
        clients_.emplace(info->m_hConn, std::move(client));
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        drop_client(info->m_hConn, info->m_info.m_szEndDebug, true);
        break;
    default:
        break;
    }
}

void Relay::poll_messages() {
    for (;;) {
        SteamNetworkingMessage_t* message = nullptr;
        const int count = sockets_->ReceiveMessagesOnPollGroup(poll_group_, &message, 1);
        if (count <= 0)
            return;
        const auto client = clients_.find(message->m_conn);
        if (client != clients_.end() && message->m_cbSize <= protocol::kMaxFrameBytes) {
            const auto* bytes = static_cast<const uint8_t*>(message->m_pData);
            process_message(client->second,
                std::span<const uint8_t>(bytes, static_cast<size_t>(message->m_cbSize)));
        } else if (client != clients_.end()) {
            const auto connection = client->first;
            message->Release();
            reject_and_drop(connection, "frame exceeds server limit");
            continue;
        }
        message->Release();
    }
}

void Relay::process_message(Client& client, std::span<const uint8_t> bytes) {
    protocol::FrameView frame;
    std::string error;
    if (!protocol::decode_frame(bytes, frame, error)) {
        reject_and_drop(client.connection, "malformed protocol frame: " + error);
        return;
    }
    client.last_seen = std::chrono::steady_clock::now();
    if (!client.joined) {
        if (frame.type != protocol::MessageType::Hello) {
            reject_and_drop(client.connection, "hello must be the first message");
        } else {
            process_hello(client, frame);
        }
        return;
    }
    switch (frame.type) {
    case protocol::MessageType::ManifestUpdate:
        process_manifest_update(client, frame);
        break;
    case protocol::MessageType::ModuleData:
        process_module_data(client, frame);
        break;
    case protocol::MessageType::Pong: {
        uint64_t nonce = 0;
        if (!protocol::decode_nonce(frame, nonce, error)) {
            reject_and_drop(client.connection, "malformed pong");
        }
        break;
    }
    case protocol::MessageType::Ping: {
        uint64_t nonce = 0;
        std::vector<uint8_t> pong;
        if (protocol::decode_nonce(frame, nonce, error) &&
            protocol::encode_nonce(protocol::MessageType::Pong, nonce, pong, error))
        {
            send_frame(client.connection, pong, false);
        }
        break;
    }
    default:
        reject_and_drop(client.connection, "client sent a server-only protocol message");
        break;
    }
}

void Relay::process_hello(Client& client, const protocol::FrameView& frame) {
    protocol::Hello hello;
    std::string error;
    if (!protocol::decode(frame, hello, error) || hello.peer.display_name.empty()) {
        reject_and_drop(client.connection, "invalid hello: " + error);
        return;
    }
    hello.peer.peer_id = 0;  // Never trust a client-provided identity.
    if (!join_room(client, std::move(hello), error)) {
        reject_and_drop(client.connection, error);
    }
}

bool Relay::join_room(Client& client, protocol::Hello hello, std::string& error) {
    std::string room_code = hello.create_room ? make_room_code() : hello.room_code;
    std::transform(room_code.begin(), room_code.end(), room_code.begin(),
        [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    if (hello.create_room) {
        rooms_.try_emplace(room_code);
    }
    const auto room_it = rooms_.find(room_code);
    if (room_it == rooms_.end()) {
        error = "Room not found";
        return false;
    }
    if (room_it->second.clients.size() >= protocol::kMaxPeers) {
        error = "Room is full";
        return false;
    }
    for (const auto connection : room_it->second.clients) {
        const auto& existing = clients_.at(connection);
        const auto report = protocol::compare_manifests(hello.peer.modules, existing.peer.modules);
        if (!report.can_join) {
            error = existing.peer.display_name + ": " + report.describe();
            return false;
        }
    }

    client.joined = true;
    client.room_code = room_code;
    client.peer = std::move(hello.peer);
    do {
        client.peer.peer_id = next_peer_id_++;
    } while (client.peer.peer_id == 0);

    protocol::Welcome welcome;
    welcome.self_peer_id = client.peer.peer_id;
    welcome.room_code = room_code;
    for (const auto connection : room_it->second.clients) {
        welcome.peers.push_back(clients_.at(connection).peer);
    }
    std::vector<uint8_t> welcome_frame;
    if (!protocol::encode(welcome, welcome_frame, error) ||
        !send_frame(client.connection, welcome_frame, true))
    {
        error = "failed to send room welcome";
        return false;
    }

    std::vector<uint8_t> joined_frame;
    if (!protocol::encode_peer_joined(client.peer, joined_frame, error))
        return false;
    broadcast(room_it->second, joined_frame, client.connection, true);
    room_it->second.clients.push_back(client.connection);
    std::cout << client.peer.display_name << " joined " << room_code << " as #"
              << client.peer.peer_id << '\n';
    return true;
}

void Relay::process_manifest_update(Client& client, const protocol::FrameView& frame) {
    protocol::ManifestUpdate update;
    std::string error;
    if (!protocol::decode(frame, update, error)) {
        reject_and_drop(client.connection, "invalid manifest update: " + error);
        return;
    }
    const auto room_it = rooms_.find(client.room_code);
    if (room_it == rooms_.end())
        return;
    for (const auto connection : room_it->second.clients) {
        if (connection == client.connection)
            continue;
        const auto report =
            protocol::compare_manifests(update.modules, clients_.at(connection).peer.modules);
        if (!report.can_join) {
            reject_and_drop(client.connection,
                "Manifest reload is no longer room-compatible: " + report.describe());
            return;
        }
    }
    client.peer.modules = std::move(update.modules);
    update.peer_id = client.peer.peer_id;
    std::vector<uint8_t> stamped;
    if (protocol::encode(update, stamped, error)) {
        broadcast(room_it->second, stamped, client.connection, true);
    }
}

void Relay::process_module_data(Client& client, const protocol::FrameView& frame) {
    protocol::ModuleData message;
    std::string error;
    if (!protocol::decode(frame, message, error)) {
        reject_and_drop(client.connection, "invalid module message: " + error);
        return;
    }
    const auto* sender_module = find_module(client.peer, message.module_id);
    if (sender_module == nullptr) {
        reject_and_drop(client.connection, "message used an unregistered module id");
        return;
    }
    const bool reliable = message.delivery == protocol::Delivery::ReliableOrdered;
    if (!within_rate(client, reliable, message.payload.size())) {
        reject_and_drop(client.connection, "module message rate limit exceeded");
        return;
    }
    const auto room_it = rooms_.find(client.room_code);
    if (room_it == rooms_.end())
        return;
    message.sender_peer_id = client.peer.peer_id;
    std::vector<uint8_t> stamped;
    if (!protocol::encode(message, stamped, error))
        return;
    for (const auto connection : room_it->second.clients) {
        if (connection == client.connection)
            continue;
        const auto recipient_it = clients_.find(connection);
        if (recipient_it == clients_.end() ||
            (message.target_peer_id != 0 &&
                recipient_it->second.peer.peer_id != message.target_peer_id) ||
            !recipient_supports(recipient_it->second, *sender_module))
        {
            continue;
        }
        send_frame(connection, stamped, reliable);
    }
}

void Relay::maintain_clients() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<HSteamNetConnection, std::string>> expired;
    for (auto& [connection, client] : clients_) {
        if (!client.joined && now - client.connected_at > kHandshakeTimeout) {
            expired.emplace_back(connection, "hello timeout");
            continue;
        }
        if (client.joined && now - client.last_seen > kHeartbeatTimeout) {
            expired.emplace_back(connection, "heartbeat timeout");
            continue;
        }
        if (client.joined && now - client.last_ping > kHeartbeatInterval) {
            std::vector<uint8_t> ping;
            std::string error;
            if (protocol::encode_nonce(protocol::MessageType::Ping, random_(), ping, error)) {
                send_frame(connection, ping, false);
            }
            client.last_ping = now;
        }
    }
    for (const auto& [connection, reason] : expired) {
        reject_and_drop(connection, reason);
    }
}

std::string Relay::make_room_code() {
    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    do {
        code.clear();
        code.reserve(20);
        for (int i = 0; i < 20; ++i) {
            code.push_back(alphabet[random_() % (sizeof(alphabet) - 1)]);
        }
    } while (rooms_.contains(code));
    return code;
}

void Relay::reject_and_drop(HSteamNetConnection connection, const std::string& reason) {
    protocol::Reject reject{reason};
    std::vector<uint8_t> frame;
    std::string error;
    if (protocol::encode(reject, frame, error))
        send_frame(connection, frame, true);
    drop_client(connection, reason, true);
}

void Relay::drop_client(
    HSteamNetConnection connection, const std::string& reason, bool close_socket) {
    const auto client_it = clients_.find(connection);
    if (client_it == clients_.end()) {
        if (close_socket && sockets_ != nullptr) {
            sockets_->CloseConnection(connection, 0, reason.c_str(), false);
        }
        return;
    }
    const Client client = client_it->second;
    if (client.joined) {
        const auto room_it = rooms_.find(client.room_code);
        if (room_it != rooms_.end()) {
            auto& connections = room_it->second.clients;
            std::erase(connections, connection);
            protocol::PeerLeft left{client.peer.peer_id, reason};
            std::vector<uint8_t> frame;
            std::string error;
            if (protocol::encode(left, frame, error))
                broadcast(room_it->second, frame);
            if (connections.empty())
                rooms_.erase(room_it);
        }
        std::cout << client.peer.display_name << " left " << client.room_code << ": " << reason
                  << '\n';
    }
    clients_.erase(client_it);
    if (close_socket && sockets_ != nullptr) {
        sockets_->CloseConnection(connection, 0, reason.c_str(), true);
    }
}

bool Relay::send_frame(
    HSteamNetConnection connection, const std::vector<uint8_t>& frame, bool reliable) {
    if (reliable) {
        SteamNetConnectionRealTimeStatus_t status{};
        if (sockets_->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK ||
            status.m_cbPendingReliable > kMaxPendingReliableBytes)
        {
            return false;
        }
    }
    const int flags = reliable ?
                          k_nSteamNetworkingSend_Reliable :
                          k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay;
    return sockets_->SendMessageToConnection(connection, frame.data(),
               static_cast<uint32_t>(frame.size()), flags, nullptr) == k_EResultOK;
}

void Relay::broadcast(const Room& room, const std::vector<uint8_t>& frame,
    HSteamNetConnection except, bool reliable) {
    for (const auto connection : room.clients) {
        if (connection != except)
            send_frame(connection, frame, reliable);
    }
}

bool Relay::within_rate(Client& client, bool reliable, size_t bytes) {
    auto& window = reliable ? client.reliable_rate : client.unreliable_rate;
    const auto now = std::chrono::steady_clock::now();
    if (now - window.start >= std::chrono::seconds(1)) {
        window = {now, 0, 0};
    }
    ++window.messages;
    window.bytes += bytes;
    return reliable ? window.messages <= kReliableMessagesPerSecond &&
                          window.bytes <= kReliableBytesPerSecond :
                      window.messages <= kUnreliableMessagesPerSecond &&
                          window.bytes <= kUnreliableBytesPerSecond;
}

bool Relay::recipient_supports(
    const Client& recipient, const protocol::ModuleManifest& sender_module) const {
    const auto* recipient_module = find_module(recipient.peer, sender_module.module_id);
    return recipient_module != nullptr &&
           protocol::modules_compatible(sender_module, *recipient_module);
}

}  // namespace lantern::server
