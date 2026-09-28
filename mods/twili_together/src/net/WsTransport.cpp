#include "net/WsTransport.hpp"

#include "net/Protocol.hpp"

#include <fmt/format.h>

namespace twili::net {

WsTransport::~WsTransport() {
    close(1001, "going away");
}

bool WsTransport::open(const std::string& url, std::string& error) {
    if (svc_websocket == nullptr) {
        error = "WebSocket support is unavailable in this build";
        return false;
    }
    mods::ws::Options options;
    options.url = url;
    options.protocols = {kSubprotocol};
    options.connectTimeoutMs = kConnectTimeoutMs;
    options.keepaliveIntervalMs = kWsKeepaliveMs;
    options.maxMessageBytes = kMaxMessageBytes;
    mods::ws::Connection connection = mods::ws::connect(options);
    if (!connection) {
        error = connection.result() == MOD_INVALID_ARGUMENT ?
                    "invalid URL (plain ws:// only works for localhost; use wss:// or tcp://)" :
                    fmt::format("cannot connect ({})", static_cast<int>(connection.result()));
        return false;
    }
    mHandle = connection.handle();
    connection.detach();
    return true;
}

ModResult WsTransport::send(std::string_view text) {
    if (mHandle == 0 || !mOpen) {
        return MOD_UNAVAILABLE;
    }
    return svc_websocket->send(mod_ctx, mHandle, WEBSOCKET_MESSAGE_TEXT, text.data(), text.size());
}

void WsTransport::close(uint16_t code, std::string_view reason) {
    if (mHandle != 0 && svc_websocket != nullptr) {
        const std::string text{reason};
        svc_websocket->close(mod_ctx, mHandle, code, text.c_str());
    }
    mHandle = 0;
    mOpen = false;
}

void WsTransport::onEvent(const mods::ws::Event& ev, std::deque<TransportEvent>& out) {
    switch (ev.type) {
    case WEBSOCKET_EVENT_OPEN:
        mOpen = true;
        out.push_back({TransportEvent::Type::Open, {}});
        break;
    case WEBSOCKET_EVENT_MESSAGE:
        if (ev.messageKind == WEBSOCKET_MESSAGE_TEXT) {
            out.push_back({TransportEvent::Type::Message,
                std::string(reinterpret_cast<const char*>(ev.data.data()), ev.data.size())});
        }
        break;
    case WEBSOCKET_EVENT_CLOSED: {
        std::string reason{ev.error != WEBSOCKET_ERROR_NONE ? ev.message : ev.closeReason};
        if (reason.empty()) {
            reason = fmt::format("closed ({})", ev.closeCode);
        }
        mHandle = 0;
        mOpen = false;
        out.push_back({TransportEvent::Type::Closed, std::move(reason)});
        break;
    }
    default:
        break;
    }
}

}  // namespace twili::net
