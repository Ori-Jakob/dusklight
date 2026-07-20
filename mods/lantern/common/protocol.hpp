#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lantern::protocol {

inline constexpr uint32_t kMagic = 0x4E544E4Cu;  // "LNTN" on the wire.
inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr size_t kMaxFrameBytes = 64 * 1024;
inline constexpr size_t kMaxModulePayloadBytes = 32 * 1024;
inline constexpr size_t kMaxModules = 128;
inline constexpr size_t kMaxPeers = 16;
inline constexpr size_t kMaxNameBytes = 48;
inline constexpr size_t kMaxRoomCodeBytes = 32;
inline constexpr size_t kMaxIdBytes = 96;
inline constexpr size_t kMaxVersionBytes = 48;
inline constexpr size_t kMaxTagBytes = 128;
inline constexpr size_t kMaxReportBytes = 4096;

enum class Delivery : uint8_t {
    ReliableOrdered = 0,
    UnreliableLatest = 1,
};

enum ModuleFlags : uint32_t {
    ModuleOptional = 0,
    ModuleRequired = 1u << 0u,
    ModuleClientOnly = 1u << 1u,
    ModuleCosmetic = 1u << 2u,
};

struct ModuleManifest {
    std::string module_id;
    std::string owner_mod_id;
    std::string owner_mod_version;
    std::string display_version;
    uint16_t protocol_major = 1;
    uint16_t protocol_minor = 0;
    uint16_t minimum_peer_minor = 0;
    uint32_t flags = ModuleOptional;
    std::string compatibility_tag;
    std::string distribution_id;
};

struct PeerManifest {
    uint64_t peer_id = 0;
    std::string display_name;
    uint32_t color_rgb = 0xFFFFFF;
    std::vector<ModuleManifest> modules;
};

enum class MessageType : uint16_t {
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    PeerJoined = 4,
    PeerLeft = 5,
    ManifestUpdate = 6,
    ModuleData = 7,
    Ping = 8,
    Pong = 9,
};

struct FrameView {
    MessageType type{};
    std::span<const uint8_t> payload;
};

struct Hello {
    bool create_room = false;
    std::string room_code;
    PeerManifest peer;
};

struct Welcome {
    uint64_t self_peer_id = 0;
    std::string room_code;
    std::vector<PeerManifest> peers;
};

struct Reject {
    std::string reason;
};

struct PeerLeft {
    uint64_t peer_id = 0;
    std::string reason;
};

struct ManifestUpdate {
    uint64_t peer_id = 0;
    std::vector<ModuleManifest> modules;
};

struct ModuleData {
    uint64_t sender_peer_id = 0;
    uint64_t target_peer_id = 0;
    Delivery delivery = Delivery::ReliableOrdered;
    uint32_t sequence = 0;
    std::string module_id;
    std::vector<uint8_t> payload;
};

struct CompatibilityIssue {
    std::string module_id;
    std::string detail;
};

struct CompatibilityReport {
    bool can_join = true;
    std::vector<CompatibilityIssue> issues;
    std::vector<std::string> compatible_modules;

    std::string describe() const;
};

bool encode(const Hello& message, std::vector<uint8_t>& out, std::string& error);
bool encode(const Welcome& message, std::vector<uint8_t>& out, std::string& error);
bool encode(const Reject& message, std::vector<uint8_t>& out, std::string& error);
bool encode_peer_joined(const PeerManifest& peer, std::vector<uint8_t>& out, std::string& error);
bool encode(const PeerLeft& message, std::vector<uint8_t>& out, std::string& error);
bool encode(const ManifestUpdate& message, std::vector<uint8_t>& out, std::string& error);
bool encode(const ModuleData& message, std::vector<uint8_t>& out, std::string& error);
bool encode_nonce(MessageType type, uint64_t nonce, std::vector<uint8_t>& out, std::string& error);

bool decode_frame(std::span<const uint8_t> bytes, FrameView& out, std::string& error);
bool decode(const FrameView& frame, Hello& out, std::string& error);
bool decode(const FrameView& frame, Welcome& out, std::string& error);
bool decode(const FrameView& frame, Reject& out, std::string& error);
bool decode_peer_joined(const FrameView& frame, PeerManifest& out, std::string& error);
bool decode(const FrameView& frame, PeerLeft& out, std::string& error);
bool decode(const FrameView& frame, ManifestUpdate& out, std::string& error);
bool decode(const FrameView& frame, ModuleData& out, std::string& error);
bool decode_nonce(const FrameView& frame, uint64_t& out, std::string& error);

bool validate_manifest(const std::vector<ModuleManifest>& manifest, std::string& error);
bool modules_compatible(
    const ModuleManifest& left, const ModuleManifest& right, std::string* reason = nullptr);
CompatibilityReport compare_manifests(
    const std::vector<ModuleManifest>& local, const std::vector<ModuleManifest>& remote);
bool sequence_newer(uint32_t candidate, uint32_t previous);

}  // namespace lantern::protocol
