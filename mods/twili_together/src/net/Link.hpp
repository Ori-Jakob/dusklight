#pragma once

#include "net/Transport.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace twili::net {

class TcpTransport;
class WsTransport;

enum class LinkState : uint8_t { Idle, Connecting, Open, Backoff };

// Reliable packets wait in order while the transport cannot take them; droppable ones do not.
enum class Delivery : uint8_t { Reliable, Droppable };

struct LinkEvent {
    enum class Type : uint8_t { Opened, Message, Closed };
    Type type = Type::Message;
    nlohmann::json packet;
    std::string reason;
    // Closed: a reconnect is scheduled.
    bool retrying = false;
};

// The connection to the relay: transport by URL scheme, reconnect with backoff, outbound backlog.
class Link {
public:
    ~Link();

    // False with `error` set if the URL is unusable.
    bool start(const std::string& url, bool autoReconnect, std::string& error);
    // A requested disconnect: closes with 1000 and does not reconnect.
    void stop();
    // The session joined its room; the backoff restarts once it stays joined.
    void markJoined();
    void setAutoReconnect(bool on) { mAutoReconnect = on; }

    // Polls the services and timers; call once per tick.
    void pump();
    bool next(LinkEvent& out);
    // False when the packet was dropped.
    bool send(const nlohmann::json& packet, Delivery delivery = Delivery::Reliable);

    LinkState state() const { return mState; }
    bool open() const { return mState == LinkState::Open; }
    const std::string& url() const { return mUrl; }
    const char* transportName() const;
    int attempt() const { return mAttempt; }
    // Seconds until the next reconnect while backing off.
    int retryInSeconds() const;

private:
    bool openTransport(std::string& error);
    void dropTransport();
    void onClosed(std::string reason);
    void flushBacklog();

    std::unique_ptr<Transport> mTransport;
    WsTransport* mWs = nullptr;
    TcpTransport* mTcp = nullptr;
    std::deque<TransportEvent> mRaw;
    std::deque<LinkEvent> mEvents;
    std::deque<std::string> mBacklog;
    std::string mUrl;
    LinkState mState = LinkState::Idle;
    bool mAutoReconnect = true;
    bool mEverJoined = false;
    int mAttempt = 0;
    std::chrono::steady_clock::time_point mJoinedAt{};
    std::chrono::steady_clock::time_point mRetryAt{};
};

}  // namespace twili::net
