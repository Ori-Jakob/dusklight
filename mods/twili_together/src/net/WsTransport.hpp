#pragma once

#include "net/Transport.hpp"

#include <mods/svc/websocket.hpp>

namespace twili::net {

// ws:// (localhost only) and wss:// on WebSocketService.
class WsTransport final : public Transport {
public:
    ~WsTransport() override;
    bool open(const std::string& url, std::string& error) override;
    ModResult send(std::string_view text) override;
    void close(uint16_t code, std::string_view reason) override;
    uint64_t handle() const override { return mHandle; }
    const char* name() const override { return "ws"; }

    void onEvent(const mods::ws::Event& ev, std::deque<TransportEvent>& out);

private:
    WebSocketHandle mHandle = 0;
    bool mOpen = false;
};

}  // namespace twili::net
