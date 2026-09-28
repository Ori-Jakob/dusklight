#include "net/TcpTransport.hpp"

#include "net/Protocol.hpp"

#include <fmt/format.h>

#include <cstring>

namespace twili::net {
namespace {

constexpr size_t kHeaderBytes = 4;

}  // namespace

TcpTransport::~TcpTransport() {
    close(1001, "going away");
}

bool TcpTransport::open(const std::string& url, std::string& error) {
    if (svc_net == nullptr) {
        error = "TCP support is unavailable in this build";
        return false;
    }
    std::string endpoint = url;
    const size_t slash = endpoint.find('/', 6);
    if (slash != std::string::npos) {
        endpoint.resize(slash);
    }
    NetConnectDesc desc = NET_CONNECT_DESC_INIT;
    desc.connect_timeout_ms = kConnectTimeoutMs;
    desc.max_send_queue_bytes = kMaxSendQueueBytes;
    desc.no_delay = true;
    mods::net::Socket socket = mods::net::connect(endpoint, desc);
    if (!socket) {
        error = socket.result() == MOD_INVALID_ARGUMENT ?
                    "invalid address (use tcp://host:port)" :
                    fmt::format("cannot connect ({})", static_cast<int>(socket.result()));
        return false;
    }
    mHandle = socket.handle();
    socket.detach();
    mInbox.clear();
    return true;
}

ModResult TcpTransport::send(std::string_view text) {
    if (mHandle == 0 || !mOpen) {
        return MOD_UNAVAILABLE;
    }
    if (text.empty() || text.size() > kMaxMessageBytes) {
        return MOD_INVALID_ARGUMENT;
    }
    std::string frame(kHeaderBytes + text.size(), '\0');
    const uint32_t size = static_cast<uint32_t>(text.size());
    for (size_t i = 0; i < kHeaderBytes; ++i) {
        frame[i] = static_cast<char>((size >> (8 * i)) & 0xFF);
    }
    std::memcpy(frame.data() + kHeaderBytes, text.data(), text.size());
    const ModResult result = svc_net->send(mod_ctx, mHandle, frame.data(), frame.size());
    return result == MOD_OK || result == MOD_UNAVAILABLE ? result : MOD_CONFLICT;
}

void TcpTransport::close(uint16_t, std::string_view) {
    if (mHandle != 0 && svc_net != nullptr) {
        svc_net->close(mod_ctx, mHandle);
    }
    mHandle = 0;
    mOpen = false;
    mInbox.clear();
}

void TcpTransport::fail(std::string reason, std::deque<TransportEvent>& out) {
    close(1002, reason);
    out.push_back({TransportEvent::Type::Closed, std::move(reason)});
}

void TcpTransport::onEvent(const mods::net::Event& ev, std::deque<TransportEvent>& out) {
    const auto now = std::chrono::steady_clock::now();
    switch (ev.type) {
    case NET_EVENT_CONNECTED:
        mOpen = true;
        mLastReceived = now;
        mLastKeepalive = now;
        out.push_back({TransportEvent::Type::Open, {}});
        break;
    case NET_EVENT_STREAM_DATA: {
        mLastReceived = now;
        mInbox.append(reinterpret_cast<const char*>(ev.data.data()), ev.data.size());
        size_t pos = 0;
        while (mInbox.size() - pos >= kHeaderBytes) {
            uint32_t size = 0;
            for (size_t i = 0; i < kHeaderBytes; ++i) {
                size |= static_cast<uint32_t>(static_cast<uint8_t>(mInbox[pos + i])) << (8 * i);
            }
            if (size == 0 || size > kMaxMessageBytes) {
                fail(fmt::format("bad frame from the relay ({} bytes)", size), out);
                return;
            }
            if (mInbox.size() - pos - kHeaderBytes < size) {
                break;
            }
            out.push_back(
                {TransportEvent::Type::Message, mInbox.substr(pos + kHeaderBytes, size)});
            pos += kHeaderBytes + size;
        }
        mInbox.erase(0, pos);
        break;
    }
    case NET_EVENT_CLOSED: {
        std::string reason{ev.message};
        if (reason.empty()) {
            reason = ev.error != NET_ERROR_NONE ?
                         fmt::format("network error {}", static_cast<int>(ev.error)) :
                         std::string("connection closed");
        }
        mHandle = 0;
        mOpen = false;
        mInbox.clear();
        out.push_back({TransportEvent::Type::Closed, std::move(reason)});
        break;
    }
    default:
        break;
    }
}

void TcpTransport::tick(std::deque<TransportEvent>& out) {
    if (!mOpen) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastReceived > std::chrono::milliseconds(kTcpTimeoutMs)) {
        fail("the relay stopped answering", out);
        return;
    }
    if (now - mLastKeepalive >= std::chrono::milliseconds(kTcpKeepaliveMs)) {
        mLastKeepalive = now;
        send(R"({"type":"KEEPALIVE"})");
    }
}

}  // namespace twili::net
