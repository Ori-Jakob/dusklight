#pragma once

#include <mods/api.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace twili::net {

struct TransportEvent {
    enum class Type : uint8_t { Open, Message, Closed };
    Type type = Type::Message;
    std::string data;  // Message: one JSON text; Closed: the reason
};

// One connection to the relay; the Link routes service events to it by handle.
class Transport {
public:
    virtual ~Transport() = default;
    // Starts connecting. False with `error` set if the URL cannot be used.
    virtual bool open(const std::string& url, std::string& error) = 0;
    // MOD_UNAVAILABLE before Open, MOD_CONFLICT while the outbound queue is full.
    virtual ModResult send(std::string_view text) = 0;
    virtual void close(uint16_t code, std::string_view reason) = 0;
    virtual uint64_t handle() const = 0;
    virtual const char* name() const = 0;
    // Called every pump while this transport is current.
    virtual void tick(std::deque<TransportEvent>&) {}
};

}  // namespace twili::net
