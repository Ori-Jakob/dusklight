#include "internal.hpp"
#include "overrides.hpp"
#include "registry.hpp"

#include <dolphin/dvd.h>

#include "dusk/config.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/utilities.hpp"
#include "dusk/version.hpp"
#include "mods/svc/mods.h"

#include "JSystem/JKernel/JKRArchive.h"

#include <aurora/dvd.h>
#include <borealis/log.hpp>
#include <fmt/format.h>
#include <picosha2.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dusk::mods::svc {
namespace {

constexpr borealis::Log Log{"dusk::mods::mods"};

// Other mods' settings that ModsService hands out: the cosmetics a player would show others.
constexpr std::string_view kCosmeticsModId = "dev.twilitrealm.cosmetics";
constexpr std::array<std::string_view, 24> kCosmeticsVars = {
    "herosTunicCapColor",
    "herosTunicTorsoColor",
    "herosTunicSkirtColor",
    "zoraArmorCapColor",
    "zoraArmorHelmetColor",
    "zoraArmorTorsoColor",
    "zoraArmorScalesColor",
    "zoraArmorFlippersColor",
    "lanternGlowColor",
    "woodenSwordColor",
    "ordonSwordBladeColor",
    "ordonSwordHandleColor",
    "msBladeColor",
    "msHandleColor",
    "lightSwordGlowColor",
    "boomerangColor",
    "ironBootsColor",
    "spinnerColor",
    "midnaHairBaseColor",
    "midnaHairTipsColor",
    "midnaChargeRingColor",
    "linkHairColor",
    "wolfLinkColor",
    "eponaColor",
};

bool is_shared_var(std::string_view modId, std::string_view name) {
    return modId == kCosmeticsModId && std::ranges::find(kCosmeticsVars, name) != kCosmeticsVars.end();
}

enum class SourceType : uint8_t {
    BundleFile,
    Buffer,
    BaseAlias,
    RawTexture,
};

struct HashSlot {
    std::mutex mutex;
    bool ready = false;
    std::array<u8, 32> sha{};
};

// Immutable once published; read from any thread.
struct OverrideRecord {
    std::string modId;
    uint64_t id = 0;
    ModOverrideKind kind = MOD_OVR_DISC_FILE;
    bool runtime = false;
    bool wins = false;
    std::string discPath;
    std::string archivePath;
    std::string aliasOf;
    std::string bundlePath;
    uint64_t size = 0;
    TextureKey textureKey = TEXTURE_KEY_INIT;
    bool textureRaw = false;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t textureMipCount = 0;
    uint32_t textureGxFormat = 0;
    // Aurora picks the highest priority, then the latest registration.
    std::pair<int32_t, uint64_t> textureRank;

    SourceType source = SourceType::BundleFile;
    std::shared_ptr<ModBundle> bundle;
    std::shared_ptr<const std::vector<u8>> buffer;
    std::shared_ptr<const void> keepalive;
    std::span<const u8> raw;
    int32_t baseEntryNum = -1;
    std::shared_ptr<HashSlot> hash;
};

struct Snapshot {
    uint64_t generation = 0;
    std::vector<OverrideRecord> records;
    std::unordered_map<uint64_t, size_t> byId;
};

// Published snapshot; the pointer is guarded, the snapshot itself is immutable.
std::mutex s_snapshotMutex;
std::shared_ptr<const Snapshot> s_snapshot;
bool s_used = false;

// Game thread only: stable ids and cached hashes by source identity.
struct Identity {
    uint64_t id = 0;
    std::shared_ptr<HashSlot> hash;
    bool seen = false;
};
std::map<std::pair<const void*, std::string>, Identity> s_identities;
uint64_t s_nextOverrideId = 1;
uint64_t s_lifecycleGeneration = 0;

struct OwnedBuffer {
    const LoadedMod* owner = nullptr;
    std::vector<u8> bytes;
};
std::mutex s_bufferMutex;
std::unordered_map<const void*, OwnedBuffer> s_buffers;

struct Watcher {
    ModsChangedFn fn = nullptr;
    void* userData = nullptr;
    uint64_t order = 0;
    uint64_t lastGeneration = 0;
};
SlotMap<Watcher> s_watchers;
uint64_t s_nextWatchOrder = 0;

char s_gameId[7] = {};

uint64_t current_generation() {
    return s_lifecycleGeneration + overlay_registrations_generation() +
           texture_registrations_generation();
}

std::string lowercase(std::string_view text) {
    std::string out{text};
    for (auto& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
        }
    }
    return out;
}

// The archive an entry overlay path belongs to: the nearest "<dir>.arc" the file table has.
std::string find_archive(
    const std::string& discPath, std::unordered_map<std::string, bool>& known) {
    for (auto slash = discPath.rfind('/'); slash != std::string::npos && slash > 0;
         slash = discPath.rfind('/', slash - 1))
    {
        std::string candidate = discPath.substr(0, slash) + ".arc";
        const auto [it, inserted] = known.try_emplace(lowercase(candidate), false);
        if (inserted) {
            it->second = DVDConvertPathToEntrynum(candidate.c_str()) >= 0;
        }
        if (it->second) {
            return candidate;
        }
    }
    return {};
}

void assign_identity(OverrideRecord& record, const void* owner, std::string name) {
    auto& identity = s_identities[{owner, std::move(name)}];
    if (identity.id == 0) {
        identity.id = s_nextOverrideId++;
        identity.hash = std::make_shared<HashSlot>();
    }
    identity.seen = true;
    record.id = identity.id;
    record.hash = identity.hash;
}

TextureKey to_texture_key(const aurora::texture::ReplacementKey& key) {
    TextureKey out = TEXTURE_KEY_INIT;
    if (const auto* pointer = std::get_if<aurora::texture::TexturePointerKey>(&key)) {
        out.kind = TEXTURE_KEY_POINTER;
        out.pointer = pointer->data;
    } else if (const auto* source = std::get_if<aurora::texture::TextureSourceKey>(&key)) {
        out.kind = TEXTURE_KEY_SOURCE;
        out.texture_hash = source->textureHash;
        out.tlut_hash = source->tlutHash;
        out.width = source->width;
        out.height = source->height;
        out.gx_format = source->format;
        out.has_tlut = source->hasTlut;
    }
    return out;
}

std::string texture_key_string(const TextureKey& key) {
    if (key.kind == TEXTURE_KEY_POINTER) {
        return fmt::format("p{}", key.pointer);
    }
    return fmt::format("s{:x}.{:x}.{}x{}.{}.{}", key.texture_hash, key.tlut_hash, key.width,
        key.height, key.gx_format, key.has_tlut);
}

OverrideRecord disc_record(const LoadedMod& mod, std::string discPath,
    std::unordered_map<std::string, bool>& archives) {
    OverrideRecord record;
    record.modId = mod.metadata.id;
    record.archivePath = find_archive(discPath, archives);
    record.kind = record.archivePath.empty() ? MOD_OVR_DISC_FILE : MOD_OVR_ARC_ENTRY;
    record.discPath = std::move(discPath);
    return record;
}

OverrideRecord texture_record(const LoadedMod& mod, const TextureOverrideInfo& info) {
    OverrideRecord record;
    record.modId = mod.metadata.id;
    record.kind = MOD_OVR_TEXTURE;
    record.runtime = info.handle != 0;
    record.textureKey = to_texture_key(info.key);
    record.textureRank = {info.priority, info.registrationId};
    if (info.bundle != nullptr) {
        record.source = SourceType::BundleFile;
        record.bundle = info.bundle;
        record.bundlePath = info.bundlePath;
        record.size = info.bundle->getFileSize(info.bundlePath);
    } else {
        record.source = SourceType::RawTexture;
        record.keepalive = info.keepalive;
        record.raw = info.raw;
        record.size = info.raw.size();
        record.textureRaw = true;
        record.textureWidth = info.width;
        record.textureHeight = info.height;
        record.textureMipCount = info.mipCount;
        record.textureGxFormat = info.gxFormat;
    }
    return record;
}

void collect_mod(LoadedMod& mod, std::vector<OverrideRecord>& records,
    std::unordered_map<std::string, bool>& archives) {
    for (auto& file : overlay_static_entries(mod)) {
        auto record = disc_record(mod, std::move(file.discPath), archives);
        record.source = SourceType::BundleFile;
        record.bundle = mod.bundle;
        record.bundlePath = file.bundlePath;
        record.size = file.size;
        assign_identity(record, mod.bundle.get(), "o:" + file.bundlePath);
        records.push_back(std::move(record));
    }

    const auto textures = texture_entries(mod);
    for (const auto& info : textures) {
        if (info.handle == 0) {
            auto record = texture_record(mod, info);
            assign_identity(record, info.bundle.get(), "t:" + info.bundlePath);
            records.push_back(std::move(record));
        }
    }

    for (auto& entry : overlay_runtime_entries(mod)) {
        auto record = disc_record(mod, std::move(entry.discPath), archives);
        record.runtime = true;
        record.size = entry.size;
        if (!entry.aliasOf.empty()) {
            record.source = SourceType::BaseAlias;
            record.aliasOf = std::move(entry.aliasOf);
            record.baseEntryNum = entry.baseEntryNum;
        } else if (entry.buffer != nullptr) {
            record.source = SourceType::Buffer;
            record.buffer = std::move(entry.buffer);
        } else {
            record.source = SourceType::BundleFile;
            record.bundle = mod.bundle;
            record.bundlePath = std::move(entry.bundlePath);
        }
        assign_identity(record, &mod, fmt::format("ro:{}", entry.handle));
        records.push_back(std::move(record));
    }

    for (const auto& info : textures) {
        if (info.handle != 0) {
            auto record = texture_record(mod, info);
            assign_identity(record, &mod, fmt::format("rt:{}", info.handle));
            records.push_back(std::move(record));
        }
    }
}

// Records come in overlay set order, where later claims of a path win.
void mark_winners(std::vector<OverrideRecord>& records) {
    std::unordered_map<std::string, size_t> pathWinner;
    std::unordered_map<std::string, size_t> textureWinner;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        if (record.kind != MOD_OVR_TEXTURE) {
            pathWinner[lowercase(record.discPath)] = i;
            continue;
        }
        const auto [it, inserted] = textureWinner.try_emplace(texture_key_string(record.textureKey), i);
        if (!inserted && record.textureRank > records[it->second].textureRank) {
            it->second = i;
        }
    }
    for (const auto& winner : pathWinner | std::views::values) {
        records[winner].wins = true;
    }
    for (const auto& winner : textureWinner | std::views::values) {
        records[winner].wins = true;
    }
}

std::shared_ptr<const Snapshot> build_snapshot(uint64_t generation) {
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->generation = generation;
    for (auto& identity : s_identities | std::views::values) {
        identity.seen = false;
    }

    std::unordered_map<std::string, bool> archives;
    for (auto& mod : ModLoader::instance().active_mods()) {
        const auto first = snapshot->records.size();
        try {
            collect_mod(mod, snapshot->records, archives);
        } catch (const std::exception& e) {
            Log.error("[{}] listing overrides failed: {}", mod.metadata.id, e.what());
            snapshot->records.resize(first);
        }
    }
    mark_winners(snapshot->records);

    std::erase_if(s_identities, [](const auto& item) { return !item.second.seen; });
    for (size_t i = 0; i < snapshot->records.size(); ++i) {
        snapshot->byId.emplace(snapshot->records[i].id, i);
    }
    return snapshot;
}

// Game thread: rebuilds when anything changed since the last build.
std::shared_ptr<const Snapshot> fresh_snapshot() {
    s_used = true;
    const auto generation = current_generation();
    {
        std::lock_guard lock{s_snapshotMutex};
        if (s_snapshot != nullptr && s_snapshot->generation == generation) {
            return s_snapshot;
        }
    }
    auto snapshot = build_snapshot(generation);
    std::lock_guard lock{s_snapshotMutex};
    s_snapshot = snapshot;
    return snapshot;
}

// Any thread: the last published snapshot (the game thread refreshes it every frame).
std::shared_ptr<const Snapshot> published_snapshot() {
    std::lock_guard lock{s_snapshotMutex};
    return s_snapshot;
}

const OverrideRecord* find_record(
    const Snapshot* snapshot, const char* modId, uint64_t id) {
    if (snapshot == nullptr || modId == nullptr) {
        return nullptr;
    }
    const auto it = snapshot->byId.find(id);
    if (it == snapshot->byId.end() || snapshot->records[it->second].modId != modId) {
        return nullptr;
    }
    return &snapshot->records[it->second];
}

bool read_base_file(int32_t entryNum, std::vector<u8>& out) {
    const int64_t size = aurora_dvd_base_file_size(entryNum);
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    return aurora_dvd_read_base_file(entryNum, out.data(), out.size(), 0) == size;
}

// The record's bytes; `view` points at them, into `storage` when they had to be read.
bool record_bytes(
    const OverrideRecord& record, std::vector<u8>& storage, std::span<const u8>& view) {
    switch (record.source) {
    case SourceType::BundleFile:
        storage = record.bundle->readFile(record.bundlePath);
        view = storage;
        return true;
    case SourceType::Buffer:
        view = *record.buffer;
        return true;
    case SourceType::BaseAlias:
        if (!read_base_file(record.baseEntryNum, storage)) {
            return false;
        }
        view = storage;
        return true;
    case SourceType::RawTexture:
        view = record.raw;
        return true;
    }
    return false;
}

void give_buffer(const LoadedMod* owner, std::vector<u8> bytes, ModBuffer* out) {
    out->data = nullptr;
    out->size = 0;
    if (bytes.empty()) {
        return;
    }
    void* data = bytes.data();
    const size_t size = bytes.size();
    {
        std::lock_guard lock{s_bufferMutex};
        s_buffers.emplace(data, OwnedBuffer{.owner = owner, .bytes = std::move(bytes)});
    }
    out->data = data;
    out->size = size;
}

bool valid_buffer(const ModBuffer* buffer) {
    return buffer != nullptr && buffer->struct_size >= sizeof(ModBuffer);
}

uint64_t mods_generation(ModContext*) {
    s_used = true;
    return current_generation();
}

ModResult mods_list_mods(ModContext* context, ModInfoFn fn, void* userData) {
    if (mod_from_context(context) == nullptr || fn == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    uint32_t order = 0;
    for (const auto& mod : ModLoader::instance().mods()) {
        const uint32_t loadOrder = order++;
        if (!mod.active) {
            continue;
        }
        const ModInfo info{
            .struct_size = sizeof(ModInfo),
            .id = mod.metadata.id.c_str(),
            .name = mod.metadata.name.c_str(),
            .version = mod.metadata.version.c_str(),
            .author = mod.metadata.author.c_str(),
            .manifest_json = mod.manifestJson.c_str(),
            .manifest_len = mod.manifestJson.size(),
            .load_order = loadOrder,
            .has_native = mod.native != nullptr,
            .has_script = mod.runtime.has_value(),
        };
        if (!fn(userData, &info)) {
            break;
        }
    }
    return MOD_OK;
}

ModResult mods_list_overrides(
    ModContext* context, const char* modId, ModOverrideFn fn, void* userData) {
    if (mod_from_context(context) == nullptr || modId == nullptr || fn == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    const auto* target = ModLoader::instance().find_mod(modId);
    if (target == nullptr || !target->active) {
        return MOD_UNAVAILABLE;
    }

    const auto snapshot = fresh_snapshot();
    const auto cstr = [](const std::string& text) { return text.empty() ? nullptr : text.c_str(); };
    for (const auto& record : snapshot->records) {
        if (record.modId != modId) {
            continue;
        }
        const ModOverride info{
            .struct_size = sizeof(ModOverride),
            .id = record.id,
            .kind = record.kind,
            .runtime = record.runtime,
            .wins = record.wins,
            .disc_path = cstr(record.discPath),
            .archive_path = cstr(record.archivePath),
            .alias_of = cstr(record.aliasOf),
            .bundle_path = cstr(record.bundlePath),
            .size = record.size,
            .texture_key = record.textureKey,
            .texture_raw = record.textureRaw,
            .texture_width = record.textureWidth,
            .texture_height = record.textureHeight,
            .texture_mip_count = record.textureMipCount,
            .texture_gx_format = record.textureGxFormat,
        };
        if (!fn(userData, &info)) {
            break;
        }
    }
    return MOD_OK;
}

ModResult mods_hash_override(ModContext* context, const char* modId, uint64_t id, uint8_t* outSha) {
    if (mod_from_context(context) == nullptr || modId == nullptr || outSha == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    const auto snapshot = published_snapshot();
    const auto* record = find_record(snapshot.get(), modId, id);
    if (record == nullptr) {
        return MOD_UNAVAILABLE;
    }
    {
        std::lock_guard lock{record->hash->mutex};
        if (record->hash->ready) {
            std::memcpy(outSha, record->hash->sha.data(), record->hash->sha.size());
            return MOD_OK;
        }
    }

    std::vector<u8> storage;
    std::span<const u8> bytes;
    if (!record_bytes(*record, storage, bytes)) {
        return MOD_UNAVAILABLE;
    }
    std::array<u8, 32> sha{};
    picosha2::hash256(bytes.begin(), bytes.end(), sha.begin(), sha.end());
    {
        std::lock_guard lock{record->hash->mutex};
        record->hash->sha = sha;
        record->hash->ready = true;
    }
    std::memcpy(outSha, sha.data(), sha.size());
    return MOD_OK;
}

ModResult mods_read_override(ModContext* context, const char* modId, uint64_t id, ModBuffer* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || modId == nullptr || !valid_buffer(out)) {
        return MOD_INVALID_ARGUMENT;
    }
    out->data = nullptr;
    out->size = 0;
    const auto snapshot = published_snapshot();
    const auto* record = find_record(snapshot.get(), modId, id);
    if (record == nullptr) {
        return MOD_UNAVAILABLE;
    }
    std::vector<u8> storage;
    std::span<const u8> bytes;
    if (!record_bytes(*record, storage, bytes)) {
        return MOD_UNAVAILABLE;
    }
    if (bytes.data() != storage.data()) {
        storage.assign(bytes.begin(), bytes.end());
    }
    give_buffer(mod, std::move(storage), out);
    return MOD_OK;
}

ModResult mods_read_base_disc_file(ModContext* context, const char* discPath, ModBuffer* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || discPath == nullptr || !utils::is_valid_disc_path(discPath) ||
        !valid_buffer(out))
    {
        return MOD_INVALID_ARGUMENT;
    }
    out->data = nullptr;
    out->size = 0;
    std::vector<u8> bytes;
    if (!read_base_file(aurora_dvd_base_entry_num(discPath), bytes)) {
        return MOD_UNAVAILABLE;
    }
    give_buffer(mod, std::move(bytes), out);
    return MOD_OK;
}

config::ConfigVarBase* find_shared_var(const char* modId, const char* name) {
    if (modId == nullptr || name == nullptr || !is_shared_var(modId, name)) {
        return nullptr;
    }
    const auto* owner = ModLoader::instance().find_mod(modId);
    if (owner == nullptr || !owner->active) {
        return nullptr;
    }
    return config::GetConfigVar(fmt::format("mod.{}.{}", escape_mod_id_for_config(modId), name));
}

ModResult mods_get_mod_var_string(ModContext* context, const char* modId, const char* name,
    char* buffer, size_t bufferSize, size_t* outLength) {
    if (mod_from_context(context) == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    auto* var = find_shared_var(modId, name);
    if (var == nullptr) {
        return MOD_UNAVAILABLE;
    }
    const auto* stringVar = dynamic_cast<ConfigVar<std::string>*>(var);
    if (stringVar == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    const std::string& value = stringVar->getValue();
    if (outLength != nullptr) {
        *outLength = value.size();
    }
    if (buffer == nullptr) {
        return bufferSize == 0 ? MOD_OK : MOD_INVALID_ARGUMENT;
    }
    if (bufferSize <= value.size()) {
        return MOD_INVALID_ARGUMENT;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return MOD_OK;
}

ModResult mods_get_mod_var_int(
    ModContext* context, const char* modId, const char* name, int64_t* outValue) {
    if (mod_from_context(context) == nullptr || outValue == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    auto* var = find_shared_var(modId, name);
    if (var == nullptr) {
        return MOD_UNAVAILABLE;
    }
    const auto* intVar = dynamic_cast<ConfigVar<s64>*>(var);
    if (intVar == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    *outValue = intVar->getValue();
    return MOD_OK;
}

ModResult mods_watch(ModContext* context, ModsChangedFn fn, void* userData, uint64_t* outHandle) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || fn == nullptr || outHandle == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    s_used = true;
    *outHandle = s_watchers.emplace(*mod, Watcher{
                                              .fn = fn,
                                              .userData = userData,
                                              .order = s_nextWatchOrder++,
                                              .lastGeneration = current_generation(),
                                          });
    return MOD_OK;
}

ModResult mods_unwatch(ModContext* context, uint64_t handle) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    return s_watchers.erase_owned(handle, *mod) ? MOD_OK : MOD_INVALID_ARGUMENT;
}

void mods_free_buffer(ModContext*, ModBuffer* buffer) {
    if (!valid_buffer(buffer)) {
        return;
    }
    if (buffer->data != nullptr) {
        std::lock_guard lock{s_bufferMutex};
        s_buffers.erase(buffer->data);
    }
    buffer->data = nullptr;
    buffer->size = 0;
}

ModResult mods_debug_texture_source(
    ModContext* context, const void* texObj, const void* tlutObj, ModTextureSource* out) {
    if (mod_from_context(context) == nullptr || texObj == nullptr || out == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    switch (aurora::texture::find_replacement_source(
        static_cast<const GXTexObj*>(texObj), static_cast<const GXTlutObj*>(tlutObj)))
    {
    case aurora::texture::ReplacementSource::Pointer:
        *out = MOD_TEXTURE_SOURCE_POINTER;
        break;
    case aurora::texture::ReplacementSource::Source:
        *out = MOD_TEXTURE_SOURCE_SOURCE;
        break;
    default:
        *out = MOD_TEXTURE_SOURCE_NONE;
        break;
    }
    return MOD_OK;
}

ModResult mods_debug_overlay_stats(ModContext* context, ModOverlayStats* out) {
    if (mod_from_context(context) == nullptr || out == nullptr ||
        out->struct_size < sizeof(ModOverlayStats))
    {
        return MOD_INVALID_ARGUMENT;
    }
    u64 live = 0;
    u64 total = 0;
    JKRArchive::getOverlayBufferStats(&live, &total);
    out->live_entry_buffers = live;
    out->total_entry_buffers = total;
    out->overlay_syncs = overlay_sync_count();
    return MOD_OK;
}

constinit ModsService s_modsService{
    .header = SERVICE_HEADER(ModsService, MODS_SERVICE_MAJOR, MODS_SERVICE_MINOR),
    .generation = mods_generation,
    .list_mods = SERVICE_FUNCTION(mods_list_mods),
    .list_overrides = SERVICE_FUNCTION(mods_list_overrides),
    .hash_override = SERVICE_FUNCTION(mods_hash_override),
    .read_override = SERVICE_FUNCTION(mods_read_override),
    .read_base_disc_file = SERVICE_FUNCTION(mods_read_base_disc_file),
    .get_mod_var_string = SERVICE_FUNCTION(mods_get_mod_var_string),
    .get_mod_var_int = SERVICE_FUNCTION(mods_get_mod_var_int),
    .watch = SERVICE_FUNCTION(mods_watch),
    .unwatch = SERVICE_FUNCTION(mods_unwatch),
    .free_buffer = mods_free_buffer,
    .debug_texture_source = SERVICE_FUNCTION(mods_debug_texture_source),
    .debug_overlay_stats = SERVICE_FUNCTION(mods_debug_overlay_stats),
    .game_id = s_gameId,
    .disc_revision = 0,
};

void notify_watchers() {
    const auto generation = current_generation();
    struct Pending {
        uint64_t order;
        uint64_t handle;
    };
    std::vector<Pending> pending;
    s_watchers.for_each([&](uint64_t handle, const auto& entry) {
        if (entry.value.lastGeneration != generation) {
            pending.push_back({.order = entry.value.order, .handle = handle});
        }
    });
    std::ranges::sort(pending, {}, &Pending::order);

    for (const auto& item : pending) {
        auto* entry = s_watchers.find(item.handle);
        if (entry == nullptr) {
            continue;
        }
        entry->value.lastGeneration = generation;
        // A callback may watch or unwatch: copy what it needs first.
        auto* owner = entry->owner;
        const auto watcher = entry->value;
        guarded_callback(*owner, "mods watch callback",
            [&] { watcher.fn(owner->context.get(), generation, watcher.userData); });
    }
}

void mods_mod_detached(LoadedMod& mod) {
    s_watchers.erase_all(mod);
    size_t leaked = 0;
    {
        std::lock_guard lock{s_bufferMutex};
        leaked = std::erase_if(s_buffers, [&](const auto& item) { return item.second.owner == &mod; });
    }
    if (leaked != 0) {
        Log.warn("[{}] {} ModsService buffer(s) were never freed", mod.metadata.id, leaked);
    }
}

}  // namespace

constinit const ServiceModule g_modsModule{
    .id = MODS_SERVICE_ID,
    .majorVersion = MODS_SERVICE_MAJOR,
    .minorVersion = MODS_SERVICE_MINOR,
    .service = &s_modsService,
    .initialize =
        [] {
            const auto& disk = version::getDiskID();
            std::memcpy(s_gameId, disk.gameName, sizeof(disk.gameName));
            std::memcpy(s_gameId + sizeof(disk.gameName), disk.company, sizeof(disk.company));
            s_modsService.disc_revision = disk.gameVersion;
        },
    .modDetached = mods_mod_detached,
    .lifecycleApplied = [] { ++s_lifecycleGeneration; },
    .frameEnd =
        [] {
            if (!s_used) {
                return;
            }
            // Keeps off-thread reads current with the overrides as they are now.
            fresh_snapshot();
            notify_watchers();
        },
    .shutdown =
        [] {
            {
                std::lock_guard lock{s_snapshotMutex};
                s_snapshot.reset();
            }
            s_identities.clear();
            std::lock_guard lock{s_bufferMutex};
            s_buffers.clear();
        },
};

}  // namespace dusk::mods::svc
