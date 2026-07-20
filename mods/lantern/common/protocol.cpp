#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace lantern::protocol {
namespace {

class Writer {
public:
    explicit Writer(MessageType type) {
        u32(kMagic);
        u16(kProtocolVersion);
        u16(static_cast<uint16_t>(type));
        u32(0);
    }

    void u8(uint8_t value) { bytes_.push_back(value); }
    void u16(uint16_t value) {
        bytes_.push_back(static_cast<uint8_t>(value));
        bytes_.push_back(static_cast<uint8_t>(value >> 8));
    }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    void u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            bytes_.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    bool string(std::string_view value, size_t limit, std::string& error) {
        if (value.size() > limit || value.size() > std::numeric_limits<uint16_t>::max()) {
            error = "string exceeds protocol bound";
            return false;
        }
        u16(static_cast<uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    bool blob(std::span<const uint8_t> value, size_t limit, std::string& error) {
        if (value.size() > limit || value.size() > std::numeric_limits<uint32_t>::max()) {
            error = "payload exceeds protocol bound";
            return false;
        }
        u32(static_cast<uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    bool finish(std::vector<uint8_t>& out, std::string& error) {
        if (bytes_.size() > kMaxFrameBytes) {
            error = "frame exceeds protocol bound";
            return false;
        }
        const uint32_t payload = static_cast<uint32_t>(bytes_.size() - 12);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes_[8 + shift / 8] = static_cast<uint8_t>(payload >> shift);
        }
        out = std::move(bytes_);
        return true;
    }

private:
    std::vector<uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    bool u8(uint8_t& out) { return integral(out); }
    bool u16(uint16_t& out) { return integral(out); }
    bool u32(uint32_t& out) { return integral(out); }
    bool u64(uint64_t& out) { return integral(out); }
    bool string(std::string& out, size_t limit, std::string& error) {
        uint16_t size = 0;
        if (!u16(size) || size > limit || remaining() < size) {
            error = "invalid bounded string";
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool blob(std::vector<uint8_t>& out, size_t limit, std::string& error) {
        uint32_t size = 0;
        if (!u32(size) || size > limit || remaining() < size) {
            error = "invalid bounded payload";
            return false;
        }
        out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return true;
    }
    size_t remaining() const { return bytes_.size() - offset_; }
    bool done(std::string& error) const {
        if (remaining() != 0) {
            error = "trailing bytes in protocol message";
            return false;
        }
        return true;
    }

private:
    template <typename T>
    bool integral(T& out) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        out = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            out |= static_cast<T>(bytes_[offset_ + i]) << (i * 8);
        }
        offset_ += sizeof(T);
        return true;
    }

    std::span<const uint8_t> bytes_;
    size_t offset_ = 0;
};

bool write_module(Writer& writer, const ModuleManifest& module, std::string& error) {
    return writer.string(module.module_id, kMaxIdBytes, error) &&
           writer.string(module.owner_mod_id, kMaxIdBytes, error) &&
           writer.string(module.owner_mod_version, kMaxVersionBytes, error) &&
           writer.string(module.display_version, kMaxVersionBytes, error) &&
           (writer.u16(module.protocol_major), true) && (writer.u16(module.protocol_minor), true) &&
           (writer.u16(module.minimum_peer_minor), true) && (writer.u32(module.flags), true) &&
           writer.string(module.compatibility_tag, kMaxTagBytes, error) &&
           writer.string(module.distribution_id, kMaxIdBytes, error);
}

bool read_module(Reader& reader, ModuleManifest& module, std::string& error) {
    return reader.string(module.module_id, kMaxIdBytes, error) &&
           reader.string(module.owner_mod_id, kMaxIdBytes, error) &&
           reader.string(module.owner_mod_version, kMaxVersionBytes, error) &&
           reader.string(module.display_version, kMaxVersionBytes, error) &&
           reader.u16(module.protocol_major) && reader.u16(module.protocol_minor) &&
           reader.u16(module.minimum_peer_minor) && reader.u32(module.flags) &&
           reader.string(module.compatibility_tag, kMaxTagBytes, error) &&
           reader.string(module.distribution_id, kMaxIdBytes, error);
}

bool write_modules(Writer& writer, const std::vector<ModuleManifest>& modules, std::string& error) {
    if (modules.size() > kMaxModules) {
        error = "too many registered modules";
        return false;
    }
    writer.u16(static_cast<uint16_t>(modules.size()));
    for (const auto& module : modules) {
        if (!write_module(writer, module, error)) {
            return false;
        }
    }
    return true;
}

bool read_modules(Reader& reader, std::vector<ModuleManifest>& modules, std::string& error) {
    uint16_t count = 0;
    if (!reader.u16(count) || count > kMaxModules) {
        error = "invalid module count";
        return false;
    }
    modules.clear();
    modules.resize(count);
    for (auto& module : modules) {
        if (!read_module(reader, module, error)) {
            return false;
        }
    }
    return validate_manifest(modules, error);
}

bool write_peer(Writer& writer, const PeerManifest& peer, std::string& error) {
    writer.u64(peer.peer_id);
    if (!writer.string(peer.display_name, kMaxNameBytes, error)) {
        return false;
    }
    writer.u32(peer.color_rgb & 0xFFFFFFu);
    return write_modules(writer, peer.modules, error);
}

bool read_peer(Reader& reader, PeerManifest& peer, std::string& error) {
    return reader.u64(peer.peer_id) && reader.string(peer.display_name, kMaxNameBytes, error) &&
           reader.u32(peer.color_rgb) && read_modules(reader, peer.modules, error);
}

bool required(const ModuleManifest& module) {
    return (module.flags & ModuleRequired) != 0 && (module.flags & ModuleClientOnly) == 0 &&
           (module.flags & ModuleCosmetic) == 0;
}

const ModuleManifest* find_module(const std::vector<ModuleManifest>& modules, std::string_view id) {
    const auto it = std::find_if(modules.begin(), modules.end(),
        [id](const ModuleManifest& candidate) { return candidate.module_id == id; });
    return it == modules.end() ? nullptr : &*it;
}

}  // namespace

std::string CompatibilityReport::describe() const {
    if (issues.empty()) {
        return "Compatible";
    }
    std::ostringstream stream;
    stream << (can_join ? "Compatible with optional differences:" : "Join blocked:");
    for (const auto& issue : issues) {
        stream << "\n- " << issue.module_id << ": " << issue.detail;
    }
    auto result = stream.str();
    if (result.size() > kMaxReportBytes) {
        result.resize(kMaxReportBytes);
    }
    return result;
}

bool encode(const Hello& message, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::Hello);
    writer.u8(message.create_room ? 1 : 0);
    if (!writer.string(message.room_code, kMaxRoomCodeBytes, error) ||
        !write_peer(writer, message.peer, error))
    {
        return false;
    }
    return writer.finish(out, error);
}

bool encode(const Welcome& message, std::vector<uint8_t>& out, std::string& error) {
    if (message.peers.size() > kMaxPeers) {
        error = "too many peers";
        return false;
    }
    Writer writer(MessageType::Welcome);
    writer.u64(message.self_peer_id);
    if (!writer.string(message.room_code, kMaxRoomCodeBytes, error)) {
        return false;
    }
    writer.u16(static_cast<uint16_t>(message.peers.size()));
    for (const auto& peer : message.peers) {
        if (!write_peer(writer, peer, error)) {
            return false;
        }
    }
    return writer.finish(out, error);
}

bool encode(const Reject& message, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::Reject);
    return writer.string(message.reason, kMaxReportBytes, error) && writer.finish(out, error);
}

bool encode_peer_joined(const PeerManifest& peer, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::PeerJoined);
    return write_peer(writer, peer, error) && writer.finish(out, error);
}

bool encode(const PeerLeft& message, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::PeerLeft);
    writer.u64(message.peer_id);
    return writer.string(message.reason, kMaxReportBytes, error) && writer.finish(out, error);
}

bool encode(const ManifestUpdate& message, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::ManifestUpdate);
    writer.u64(message.peer_id);
    return write_modules(writer, message.modules, error) && writer.finish(out, error);
}

bool encode(const ModuleData& message, std::vector<uint8_t>& out, std::string& error) {
    Writer writer(MessageType::ModuleData);
    writer.u64(message.sender_peer_id);
    writer.u64(message.target_peer_id);
    writer.u8(static_cast<uint8_t>(message.delivery));
    writer.u32(message.sequence);
    return writer.string(message.module_id, kMaxIdBytes, error) &&
           writer.blob(message.payload, kMaxModulePayloadBytes, error) && writer.finish(out, error);
}

bool encode_nonce(MessageType type, uint64_t nonce, std::vector<uint8_t>& out, std::string& error) {
    if (type != MessageType::Ping && type != MessageType::Pong) {
        error = "nonce frame must be ping or pong";
        return false;
    }
    Writer writer(type);
    writer.u64(nonce);
    return writer.finish(out, error);
}

bool decode_frame(std::span<const uint8_t> bytes, FrameView& out, std::string& error) {
    if (bytes.size() < 12 || bytes.size() > kMaxFrameBytes) {
        error = "invalid frame size";
        return false;
    }
    Reader reader(bytes);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t type = 0;
    uint32_t payload = 0;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(type) || !reader.u32(payload) ||
        magic != kMagic || version != kProtocolVersion || payload != reader.remaining())
    {
        error = "invalid frame header";
        return false;
    }
    if (type < static_cast<uint16_t>(MessageType::Hello) ||
        type > static_cast<uint16_t>(MessageType::Pong))
    {
        error = "unknown frame type";
        return false;
    }
    out.type = static_cast<MessageType>(type);
    out.payload = bytes.subspan(12);
    return true;
}

bool decode(const FrameView& frame, Hello& out, std::string& error) {
    if (frame.type != MessageType::Hello)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    uint8_t create = 0;
    if (!reader.u8(create) || create > 1 ||
        !reader.string(out.room_code, kMaxRoomCodeBytes, error) ||
        !read_peer(reader, out.peer, error))
    {
        if (error.empty())
            error = "malformed hello";
        return false;
    }
    out.create_room = create != 0;
    return reader.done(error);
}

bool decode(const FrameView& frame, Welcome& out, std::string& error) {
    if (frame.type != MessageType::Welcome)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    uint16_t count = 0;
    if (!reader.u64(out.self_peer_id) || !reader.string(out.room_code, kMaxRoomCodeBytes, error) ||
        !reader.u16(count) || count > kMaxPeers)
    {
        if (error.empty())
            error = "malformed welcome";
        return false;
    }
    out.peers.clear();
    out.peers.resize(count);
    for (auto& peer : out.peers) {
        if (!read_peer(reader, peer, error))
            return false;
    }
    return reader.done(error);
}

bool decode(const FrameView& frame, Reject& out, std::string& error) {
    if (frame.type != MessageType::Reject)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    return reader.string(out.reason, kMaxReportBytes, error) && reader.done(error);
}

bool decode_peer_joined(const FrameView& frame, PeerManifest& out, std::string& error) {
    if (frame.type != MessageType::PeerJoined)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    return read_peer(reader, out, error) && reader.done(error);
}

bool decode(const FrameView& frame, PeerLeft& out, std::string& error) {
    if (frame.type != MessageType::PeerLeft)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    return reader.u64(out.peer_id) && reader.string(out.reason, kMaxReportBytes, error) &&
           reader.done(error);
}

bool decode(const FrameView& frame, ManifestUpdate& out, std::string& error) {
    if (frame.type != MessageType::ManifestUpdate)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    return reader.u64(out.peer_id) && read_modules(reader, out.modules, error) &&
           reader.done(error);
}

bool decode(const FrameView& frame, ModuleData& out, std::string& error) {
    if (frame.type != MessageType::ModuleData)
        return error = "wrong message type", false;
    Reader reader(frame.payload);
    uint8_t delivery = 0;
    if (!reader.u64(out.sender_peer_id) || !reader.u64(out.target_peer_id) ||
        !reader.u8(delivery) || delivery > static_cast<uint8_t>(Delivery::UnreliableLatest) ||
        !reader.u32(out.sequence) || !reader.string(out.module_id, kMaxIdBytes, error) ||
        !reader.blob(out.payload, kMaxModulePayloadBytes, error))
    {
        if (error.empty())
            error = "malformed module data";
        return false;
    }
    out.delivery = static_cast<Delivery>(delivery);
    return reader.done(error);
}

bool decode_nonce(const FrameView& frame, uint64_t& out, std::string& error) {
    if (frame.type != MessageType::Ping && frame.type != MessageType::Pong) {
        return error = "wrong message type", false;
    }
    Reader reader(frame.payload);
    return reader.u64(out) && reader.done(error);
}

bool validate_manifest(const std::vector<ModuleManifest>& manifest, std::string& error) {
    if (manifest.size() > kMaxModules) {
        error = "too many modules";
        return false;
    }
    std::unordered_set<std::string> ids;
    for (const auto& module : manifest) {
        if (module.module_id.empty() || module.module_id.size() > kMaxIdBytes ||
            module.owner_mod_id.empty() || module.owner_mod_id.size() > kMaxIdBytes ||
            module.owner_mod_version.size() > kMaxVersionBytes ||
            module.display_version.size() > kMaxVersionBytes ||
            module.compatibility_tag.size() > kMaxTagBytes ||
            module.distribution_id.size() > kMaxIdBytes || module.protocol_major == 0)
        {
            error = "invalid module manifest entry";
            return false;
        }
        if (!ids.insert(module.module_id).second) {
            error = "duplicate module id: " + module.module_id;
            return false;
        }
    }
    return true;
}

bool modules_compatible(
    const ModuleManifest& left, const ModuleManifest& right, std::string* reason) {
    auto fail = [&](const char* message) {
        if (reason != nullptr)
            *reason = message;
        return false;
    };
    if (left.module_id != right.module_id)
        return fail("module IDs differ");
    if (left.protocol_major != right.protocol_major)
        return fail("protocol majors differ");
    if (left.protocol_minor < right.minimum_peer_minor ||
        right.protocol_minor < left.minimum_peer_minor)
    {
        return fail("protocol minor requirements are not mutually satisfied");
    }
    if ((!left.compatibility_tag.empty() || !right.compatibility_tag.empty()) &&
        left.compatibility_tag != right.compatibility_tag)
    {
        return fail("compatibility tags differ");
    }
    return true;
}

CompatibilityReport compare_manifests(
    const std::vector<ModuleManifest>& local, const std::vector<ModuleManifest>& remote) {
    CompatibilityReport report;
    std::unordered_set<std::string> considered;
    auto inspect = [&](const ModuleManifest& module, const std::vector<ModuleManifest>& other) {
        if (!considered.insert(module.module_id).second)
            return;
        const ModuleManifest* match = find_module(other, module.module_id);
        if (match == nullptr) {
            if (required(module)) {
                report.can_join = false;
                report.issues.push_back({module.module_id, "required module is missing"});
            }
            return;
        }
        std::string reason;
        if (modules_compatible(module, *match, &reason)) {
            report.compatible_modules.push_back(module.module_id);
        } else if (required(module) || required(*match)) {
            report.can_join = false;
            report.issues.push_back({module.module_id, std::move(reason)});
        }
    };
    for (const auto& module : local)
        inspect(module, remote);
    for (const auto& module : remote)
        inspect(module, local);
    std::sort(report.compatible_modules.begin(), report.compatible_modules.end());
    return report;
}

bool sequence_newer(uint32_t candidate, uint32_t previous) {
    return static_cast<int32_t>(candidate - previous) > 0;
}

}  // namespace lantern::protocol
