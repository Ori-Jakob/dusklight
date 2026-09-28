#include "net/Link.hpp"

#include "core/Log.hpp"
#include "net/Protocol.hpp"
#include "net/TcpTransport.hpp"
#include "net/WsTransport.hpp"

#include <algorithm>
#include <iterator>

namespace twili::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 15, 30};
// Joined this long, the next drop starts the backoff over.
constexpr auto kStableJoin = std::chrono::seconds(60);
constexpr size_t kMaxBacklog = 4096;

bool startsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

}  // namespace

Link::~Link() {
    dropTransport();
}

bool Link::start(const std::string& url, bool autoReconnect, std::string& error) {
    dropTransport();
    mRaw.clear();
    mEvents.clear();
    mBacklog.clear();
    mUrl = url;
    mAutoReconnect = autoReconnect;
    mEverJoined = false;
    mAttempt = 0;
    mJoinedAt = {};
    if (!openTransport(error)) {
        mState = LinkState::Idle;
        return false;
    }
    mState = LinkState::Connecting;
    return true;
}

bool Link::openTransport(std::string& error) {
    std::unique_ptr<Transport> transport;
    WsTransport* ws = nullptr;
    TcpTransport* tcp = nullptr;
    if (startsWith(mUrl, "ws://") || startsWith(mUrl, "wss://")) {
        auto owned = std::make_unique<WsTransport>();
        ws = owned.get();
        transport = std::move(owned);
    } else if (startsWith(mUrl, "tcp://")) {
        auto owned = std::make_unique<TcpTransport>();
        tcp = owned.get();
        transport = std::move(owned);
    } else {
        error = "unsupported URL (use wss://, ws://localhost or tcp://host:port)";
        return false;
    }
    if (!transport->open(mUrl, error)) {
        return false;
    }
    mTransport = std::move(transport);
    mWs = ws;
    mTcp = tcp;
    return true;
}

void Link::dropTransport() {
    if (mTransport) {
        mTransport->close(1000, "disconnect");
    }
    mTransport.reset();
    mWs = nullptr;
    mTcp = nullptr;
}

void Link::stop() {
    dropTransport();
    mState = LinkState::Idle;
    mBacklog.clear();
    mRaw.clear();
    mEvents.clear();
}

void Link::markJoined() {
    mEverJoined = true;
    mJoinedAt = Clock::now();
}

const char* Link::transportName() const {
    return mTransport ? mTransport->name() : "";
}

int Link::retryInSeconds() const {
    if (mState != LinkState::Backoff) {
        return 0;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(mRetryAt - Clock::now());
    return static_cast<int>(std::max<int64_t>(0, (left.count() + 999) / 1000));
}

void Link::onClosed(std::string reason) {
    mTransport.reset();
    mWs = nullptr;
    mTcp = nullptr;
    mBacklog.clear();
    LinkEvent ev;
    ev.type = LinkEvent::Type::Closed;
    ev.reason = std::move(reason);
    if (mAutoReconnect && mEverJoined) {
        if (mJoinedAt != Clock::time_point{} && Clock::now() - mJoinedAt >= kStableJoin) {
            mAttempt = 0;
        }
        mJoinedAt = {};
        const int delay = kBackoffSeconds[std::min<size_t>(mAttempt, std::size(kBackoffSeconds) - 1)];
        mAttempt++;
        mRetryAt = Clock::now() + std::chrono::seconds(delay);
        mState = LinkState::Backoff;
        ev.retrying = true;
    } else {
        mState = LinkState::Idle;
    }
    mEvents.push_back(std::move(ev));
}

void Link::pump() {
    // Every event is consumed, so a closed connection's leftovers never pile up in the services.
    mods::ws::Event wsEvent;
    while (mods::ws::poll(wsEvent)) {
        if (mWs != nullptr && wsEvent.handle != 0 && wsEvent.handle == mWs->handle()) {
            mWs->onEvent(wsEvent, mRaw);
        }
    }
    mods::net::Event netEvent;
    while (mods::net::poll(netEvent)) {
        if (mTcp != nullptr && netEvent.handle != 0 && netEvent.handle == mTcp->handle()) {
            mTcp->onEvent(netEvent, mRaw);
        }
    }
    if (mTransport) {
        mTransport->tick(mRaw);
    }

    while (!mRaw.empty()) {
        TransportEvent raw = std::move(mRaw.front());
        mRaw.pop_front();
        switch (raw.type) {
        case TransportEvent::Type::Open:
            if (mState == LinkState::Connecting) {
                mState = LinkState::Open;
                mEvents.push_back({LinkEvent::Type::Opened, {}, {}, false});
            }
            break;
        case TransportEvent::Type::Message:
            if (mState == LinkState::Open) {
                nlohmann::json packet = nlohmann::json::parse(raw.data, nullptr, false);
                if (packet.is_object()) {
                    mEvents.push_back({LinkEvent::Type::Message, std::move(packet), {}, false});
                } else {
                    TwiliLog.warn("[net] dropped malformed message from the relay");
                }
            }
            break;
        case TransportEvent::Type::Closed:
            if (mState == LinkState::Connecting || mState == LinkState::Open) {
                onClosed(std::move(raw.data));
            }
            break;
        }
    }

    if (mState == LinkState::Backoff && Clock::now() >= mRetryAt) {
        std::string error;
        TwiliLog.info("[net] reconnecting to {} (attempt {})", mUrl, mAttempt);
        if (openTransport(error)) {
            mState = LinkState::Connecting;
        } else {
            mState = LinkState::Connecting;
            onClosed(std::move(error));
        }
    }
    if (mState == LinkState::Open) {
        flushBacklog();
    }
}

bool Link::next(LinkEvent& out) {
    if (mEvents.empty()) {
        return false;
    }
    out = std::move(mEvents.front());
    mEvents.pop_front();
    return true;
}

bool Link::send(const nlohmann::json& packet, Delivery delivery) {
    if (mState != LinkState::Open || !mTransport) {
        return false;
    }
    std::string text = packet.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (delivery == Delivery::Reliable && !mBacklog.empty()) {
        if (mBacklog.size() >= kMaxBacklog) {
            TwiliLog.warn("[net] outbound backlog full, dropping the oldest packet");
            mBacklog.pop_front();
        }
        mBacklog.push_back(std::move(text));
        return true;
    }
    const ModResult result = mTransport->send(text);
    if (result == MOD_OK) {
        return true;
    }
    if (delivery == Delivery::Reliable && (result == MOD_CONFLICT || result == MOD_UNAVAILABLE)) {
        mBacklog.push_back(std::move(text));
        return true;
    }
    return false;
}

void Link::flushBacklog() {
    while (!mBacklog.empty() && mTransport) {
        if (mTransport->send(mBacklog.front()) != MOD_OK) {
            break;
        }
        mBacklog.pop_front();
    }
}

}  // namespace twili::net
