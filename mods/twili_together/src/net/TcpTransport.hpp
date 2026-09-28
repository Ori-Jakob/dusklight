#pragma once

#include "net/Transport.hpp"

#include <mods/svc/net.hpp>

#include <chrono>
#include <string>

namespace twili::net {

// tcp://host:port on NetService: every message is a 4-byte little-endian length and JSON.
class TcpTransport final : public Transport {
public:
    ~TcpTransport() override;
    bool open(const std::string& url, std::string& error) override;
    ModResult send(std::string_view text) override;
    void close(uint16_t code, std::string_view reason) override;
    uint64_t handle() const override { return mHandle; }
    const char* name() const override { return "tcp"; }
    // Sends KEEPALIVE and drops a relay that went silent.
    void tick(std::deque<TransportEvent>& out) override;

    void onEvent(const mods::net::Event& ev, std::deque<TransportEvent>& out);

private:
    void fail(std::string reason, std::deque<TransportEvent>& out);

    NetHandle mHandle = 0;
    bool mOpen = false;
    std::string mInbox;
    std::chrono::steady_clock::time_point mLastReceived{};
    std::chrono::steady_clock::time_point mLastKeepalive{};
};

}  // namespace twili::net
