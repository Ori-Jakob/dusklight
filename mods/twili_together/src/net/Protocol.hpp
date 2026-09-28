#pragma once

#include <cstddef>
#include <cstdint>

namespace twili::net {

inline constexpr int kProtocolVersion = 5;
inline constexpr const char* kApp = "twili-together";
inline constexpr const char* kSubprotocol = "twili-together.5";

inline constexpr uint32_t kConnectTimeoutMs = 5000;
inline constexpr uint32_t kWsKeepaliveMs = 10000;
inline constexpr uint32_t kTcpKeepaliveMs = 5000;
inline constexpr uint32_t kTcpTimeoutMs = 30000;
inline constexpr size_t kMaxMessageBytes = 1024 * 1024;
inline constexpr size_t kMaxSendQueueBytes = 4 * 1024 * 1024;

}  // namespace twili::net
