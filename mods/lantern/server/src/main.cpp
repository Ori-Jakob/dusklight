#include "relay.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

lantern::server::Relay* g_relay = nullptr;

void stop_relay(int) {
    if (g_relay != nullptr)
        g_relay->request_stop();
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 43384;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--port" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            unsigned parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                parsed == 0 || parsed > 65535)
            {
                std::cerr << "Invalid port\n";
                return 2;
            }
            port = static_cast<uint16_t>(parsed);
        } else if (argument == "--help") {
            std::cout << "Usage: lantern_server [--port 43384]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }

    lantern::server::Relay relay;
    g_relay = &relay;
    std::signal(SIGINT, stop_relay);
    std::signal(SIGTERM, stop_relay);
    std::string error;
    if (!relay.start(port, error)) {
        std::cerr << "Lantern relay failed: " << error << '\n';
        return 1;
    }
    relay.run();
    relay.stop();
    g_relay = nullptr;
    return 0;
}
