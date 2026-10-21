#include "dusk/render/model_registry.hpp"

#include "dusk/main.h"
#include "dusk/render/hints.hpp"
#include "dusk/render/render_bridge.hpp"

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/JUtility/JUTNameTab.h"

#include <absl/container/flat_hash_map.h>
#include <aurora/gfx.hpp>
#include <aurora/render.hpp>
#include <borealis/log.hpp>
#include <dolphin/gx/GXCommandList.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace dusk::render {
namespace {

static_assert(make_object_id(ObjectKind::J3dModel, 1) == 0x1'0000'0001ull);
static_assert(
    is_model_resource('BMDR') && !is_model_resource('BMDL') && !is_model_resource('BCK '));

constexpr borealis::Log RegistryLog{"dusk::render"};
constexpr u32 kNoFrame = UINT32_MAX;
constexpr u32 kSerialLimit = UINT32_MAX;
constexpr size_t kDrawCacheSize = 256;
constexpr u32 kCpuDeformed = AURORA_OBJ_CPU_DEFORMED;
constexpr u32 kSkinInPlaceFlag = 0x100;
constexpr size_t kArrayBaseBytes = 16;
constexpr size_t kArraySlots = GX_VA_TEX7 - GX_VA_POS + 1;

struct ImmutableArray {
    GXAttr attr = GX_VA_POS;
    const void* data = nullptr;
    u32 size = 0;
    bool operator==(const ImmutableArray&) const = default;
};

struct ArrayRelease {
    u32 generation = 0;
    std::vector<ImmutableArray> arrays;
};

// Hints of the current aurora frame may not have reached the decoder yet
struct SentCount {
    u64 total = 0;
    u64 beforeFrame = 0;
    u32 frame = kNoFrame;

    void add(u32 now, u64 n) noexcept {
        if (frame != now) {
            beforeFrame = total;
            frame = now;
        }
        total += n;
    }

    u64 settled(u32 now) const noexcept { return frame == now ? beforeFrame : total; }
};

struct DataInfo {
    u32 generation = 0;
    std::vector<MaterialTag> materials;
    std::vector<std::string> names;
    std::string archive;
    std::vector<ImmutableArray> arrays;
    std::string arrayArchive;
    const JKRHeap* heap = nullptr;
    u32 declaredFrame = kNoFrame;
    bool declared = false;
    bool verified = false;
};

struct ModelEntry {
    u32 serial = 0;
    u32 categories = AURORA_CAT_DYNAMIC;
    u32 flags = 0;
    s16 roomNo = -1;
    s8 part = -1;
    bool billboardJoints = false;
    u32 teleportFrame = kNoFrame;
    u32 lastDrawFrame = kNoFrame;
    u32 factsFrame = kNoFrame;
    ShapeFacts facts;
    const J3DModelData* data = nullptr;
    DataInfo* info = nullptr;
};

enum class Warning : u8 {
    Thread,
    SerialWrap,
    UnknownModel,
    UnknownData,
    ReservedStatic,
    SuppressionUnderflow,
    Leak,
    Count,
};

struct RegistrySnapshot {
    u32 nextSerial = 0;
    u32 nextGeneration = 0;
    size_t models = 0;
    size_t data = 0;
    u32 logUntil = 0;
    u64 totalBegins = 0;
    bool operator==(const RegistrySnapshot&) const = default;
};

const MaterialTag* material_tag_at(const std::vector<MaterialTag>& tags, u32 index) noexcept {
    return index < tags.size() ? &tags[index] : nullptr;
}

ShapeFacts model_facts(J3DModel* model, const J3DModelData* data) noexcept {
    ShapeFacts facts;
    facts.skinPosCpu = model->checkFlag(J3DMdlFlag_SkinPosCpu);
    facts.deformData = model->mDeformData != nullptr;
    facts.localPositions = model->getVertexBuffer()->getCurrentVtxPos() != data->getVtxPosArray();
    return facts;
}

std::vector<ImmutableArray> select_immutable(
    std::span<const ImmutableArray> slots, bool keepPosNrm) {
    std::vector<ImmutableArray> arrays;
    for (const ImmutableArray& slot : slots) {
        if (slot.data == nullptr) {
            continue;
        }
        if (slot.size == 0 || std::ranges::any_of(arrays,
                                  [&slot](const ImmutableArray& a) { return a.data == slot.data; }))
        {
            return {};
        }
        arrays.push_back(slot);
    }
    if (!keepPosNrm) {
        std::erase_if(arrays,
            [](const ImmutableArray& a) { return a.attr == GX_VA_POS || a.attr == GX_VA_NRM; });
    }
    return arrays;
}

// An indexed slot with no bytes reads whatever aurora pushes after it, so the data keeps streaming
bool reads_missing_slot(const GXVtxDescList* desc, std::span<const ImmutableArray> slots) {
    if (desc == nullptr) {
        return true;
    }
    for (; desc->attr != GX_VA_NULL; ++desc) {
        if (desc->attr == GX_VA_NBT) {
            if (desc->type != GX_NONE) {
                return true;
            }
            continue;
        }
        if (desc->attr < GX_VA_POS || desc->attr > GX_VA_TEX7 ||
            (desc->type != GX_INDEX8 && desc->type != GX_INDEX16))
        {
            continue;
        }
        const ImmutableArray& slot = slots[desc->attr - GX_VA_POS];
        if (slot.data == nullptr || slot.size == 0) {
            return true;
        }
    }
    return false;
}

const void* vertex_array(const J3DVertexData& vtx, GXAttr attr) noexcept {
    switch (attr) {
    case GX_VA_POS:
        return vtx.getVtxPosArray();
    case GX_VA_NRM:
        return vtx.getVtxNrmArray();
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        return vtx.getVtxColorArray(static_cast<u8>(attr - GX_VA_CLR0));
    default:
        return vtx.getVtxTexCoordArray(static_cast<u8>(attr - GX_VA_TEX0));
    }
}

std::array<ImmutableArray, kArraySlots> empty_slots() noexcept {
    std::array<ImmutableArray, kArraySlots> slots;
    for (size_t i = 0; i < kArraySlots; ++i) {
        slots[i].attr = static_cast<GXAttr>(GX_VA_POS + i);
    }
    return slots;
}

std::vector<ImmutableArray> model_immutable_arrays(const J3DModelData* data) {
    // J3DModelData exposes its vertex data only through a non-const accessor
    const J3DVertexData& vtx = const_cast<J3DModelData*>(data)->getVertexData();
    const GXVtxAttrFmtList* fmt = vtx.getVtxAttrFmtList();
    // NBT shapes put the NBT array in the normal slot with the normal size
    if (fmt == nullptr || vtx.getVtxNBTArray() != nullptr) {
        return {};
    }
    auto slots = empty_slots();
    // Same pointers and byte sizes J3DShape::makeVtxArrayCmd bakes into every shape's array bases
    for (; fmt->attr != GX_VA_NULL; ++fmt) {
        if (fmt->attr == GX_VA_NBT) {
            return {};
        }
        if (fmt->attr < GX_VA_POS || fmt->attr > GX_VA_TEX7) {
            continue;
        }
        const void* ptr = vertex_array(vtx, fmt->attr);
        slots[fmt->attr - GX_VA_POS] = {
            fmt->attr, ptr, ptr != nullptr ? vtx.getVtxArrByteSize(fmt->attr) : 0};
    }
    for (u16 i = 0; i < data->getShapeNum(); ++i) {
        J3DShape* shape = data->getShapeNodePointer(i);
        if (shape == nullptr || reads_missing_slot(shape->getVtxDesc(), slots)) {
            return {};
        }
    }
    // setSkinDeform rewrites these positions and normals in place
    return select_immutable(
        slots, !(data->checkFlag(kSkinInPlaceFlag) && data->getWEvlpMtxNum() != 0));
}

bool vcd_declares(J3DShape* shape, const ImmutableArray& a) noexcept {
    const auto* begin = static_cast<const u8*>(shape->getVcdVatCmd());
    if (begin == nullptr) {
        return false;
    }
    const u16 command = GX_AURORA_LOAD_ARRAYBASE + (a.attr - GX_VA_POS);
    const u64 address = reinterpret_cast<uintptr_t>(a.data);
    std::array<u8, kArrayBaseBytes> encoded{};
    encoded[0] = GX_AURORA;
    encoded[1] = static_cast<u8>(command >> 8);
    encoded[2] = static_cast<u8>(command);
    for (size_t i = 0; i < 8; ++i) {
        encoded[3 + i] = static_cast<u8>(address >> (56 - 8 * i));
    }
    for (size_t i = 0; i < 4; ++i) {
        encoded[11 + i] = static_cast<u8>(a.size >> (24 - 8 * i));
    }
    encoded[15] = 1;
    const u8* end = begin + J3DShape::kVcdVatDLSize;
    return std::search(begin, end, encoded.begin(), encoded.end()) != end;
}

void send_declaration(const DataInfo& info) noexcept {
    for (const ImmutableArray& a : info.arrays) {
        GXAuroraSetArrayImmutable(a.data, a.size, GX_TRUE, info.generation);
    }
}

void send_release(const ArrayRelease& r) noexcept {
    for (const ImmutableArray& a : r.arrays) {
        GXAuroraReleaseArrayImmutable(a.data, r.generation);
    }
}

bool is_billboard_shape(J3DShape* shape) noexcept {
    if (shape->getMtxGroupNum() == 0) {
        return false;
    }
    const J3DShapeMtx* mtx = shape->getShapeMtx(0);
    if (mtx == nullptr) {
        return false;
    }
    const u32 type = mtx->getType();
    return type == 'SMBB' || type == 'SMYB';
}

std::string tag_summary(const std::vector<MaterialTag>& tags) {
    u32 categories = 0;
    u32 pending = 0;
    std::vector<std::string> policies;
    for (const MaterialTag& tag : tags) {
        categories |= tag.categories;
        pending |= tag.pendingCategories;
        if (tag.tevPolicy.mask != 0) {
            std::string text = tev_policy_text(tag.tevPolicy);
            if (std::ranges::find(policies, text) == policies.end()) {
                policies.push_back(std::move(text));
            }
        }
    }
    std::vector<std::string> parts;
    if (categories != 0) {
        parts.push_back(category_names(categories));
    }
    if (pending != 0) {
        parts.push_back("pending " + category_names(pending));
    }
    if (!policies.empty()) {
        std::string text = "policy";
        for (size_t i = 0; i < policies.size(); ++i) {
            text += (i == 0 ? " " : ", ") + policies[i];
        }
        parts.push_back(std::move(text));
    }
    std::string summary;
    for (const auto& part : parts) {
        summary += (summary.empty() ? "" : "; ") + part;
    }
    return summary;
}

// Entries live in deques and the maps hold indices, so cached entry pointers survive every insert
class Registry {
public:
    explicit Registry(bool quiet = false) : m_quiet(quiet) {
        m_modelIndex.reserve(8192);
        m_dataIndex.reserve(2048);
    }

    ModelEntry& initialize_model(const J3DModel* model) {
        ModelEntry& entry = model_slot(model);
        entry = ModelEntry{.serial = next_serial()};
        return entry;
    }

    DataInfo& create_data(const J3DModelData* data) {
        DataInfo& info = data_slot(data);
        info = DataInfo{.generation = next_generation()};
        return info;
    }

    DataInfo& known_data(const J3DModelData* data) {
        const auto it = m_dataIndex.find(data);
        if (it != m_dataIndex.end()) {
            return m_data[it->second];
        }
        if (should_warn(Warning::UnknownData)) {
            RegistryLog.warn("render registry: met model data before its construction");
        }
        return create_data(data);
    }

    void load_data(const J3DModelData* data, const char* archive, const JKRHeap* heap) {
        DataInfo& info = known_data(data);
        info.archive.clear();
        info.names.clear();
        info.materials.clear();
        track_arrays(data, model_immutable_arrays(data), archive, heap);
        // Most actor MAxx names are never applied by the game; the six BG-object actors are M9-W6's
        if (!is_room_archive(archive)) {
            return;
        }
        const JUTNameTab* names = data->getMaterialName();
        if (names != nullptr) {
            for (u16 i = 0; i < data->getMaterialNum(); ++i) {
                log_review_name(archive, names->getName(i));
            }
        }
        if (!compute_material_tags(data, info.materials)) {
            return;
        }
        info.archive = archive;
        info.names.reserve(info.materials.size());
        u32 tagged = 0;
        for (u16 i = 0; i < info.materials.size(); ++i) {
            const char* name = names->getName(i);
            info.names.emplace_back(name != nullptr ? name : "");
            const MaterialTag& tag = info.materials[i];
            if (tag.categories != 0 || tag.pendingCategories != 0 || tag.tevPolicy.mask != 0) {
                ++tagged;
            }
        }
        RegistryLog.debug("render tags: {} {} tagged materials ({})", archive, tagged,
            tag_summary(info.materials));
    }

    void declare_room(const J3DModel* model, int roomNo, int part) {
        ModelEntry& entry = known_model(model);
        entry.categories = AURORA_CAT_STATIC | AURORA_CAT_TERRAIN;
        entry.flags |= AURORA_OBJ_IMMOBILE;
        entry.roomNo = static_cast<s16>(roomNo);
        entry.part = static_cast<s8>(part);
    }

    void declare(const J3DModel* model, u32 categories, u32 flags) {
        if ((categories & AURORA_CAT_STATIC) != 0 && should_warn(Warning::ReservedStatic)) {
            RegistryLog.warn("render registry: STATIC is reserved for room models; ignored");
        }
        ModelEntry& entry = known_model(model);
        entry.categories = categories & ~AURORA_CAT_STATIC;
        entry.flags = flags;
    }

    bool mark_teleport(const J3DModel* model, u32 frame) {
        ModelEntry* entry = find_model(model);
        if (entry == nullptr) {
            return false;
        }
        entry->teleportFrame = frame;
        entry->factsFrame = kNoFrame;
        return true;
    }

    ModelEntry* find_model(const J3DModel* model) {
        const auto it = m_modelIndex.find(model);
        return it == m_modelIndex.end() ? nullptr : &m_models[it->second];
    }

    u32 serial_of(const J3DModel* model) const {
        const auto it = m_modelIndex.find(model);
        return it == m_modelIndex.end() ? 0 : m_models[it->second].serial;
    }

    u32 generation_of(const J3DModelData* data) const {
        const auto it = m_dataIndex.find(data);
        return it == m_dataIndex.end() ? 0 : m_data[it->second].generation;
    }

    // Material sort interleaves the shapes of many models, so model facts are read once per frame
    void stamp_frame(ModelEntry& entry, u32 frame, const ShapeFacts& facts,
        bool billboardJoints) const noexcept {
        entry.facts = facts;
        entry.facts.billboard = false;
        entry.facts.teleport = entry.teleportFrame == frame;
        entry.billboardJoints = billboardJoints;
        entry.factsFrame = frame;
    }

    ShapeInputs base_inputs(const ModelEntry& entry) const noexcept {
        ShapeInputs in;
        in.serial = entry.serial;
        in.generation = entry.info != nullptr ? entry.info->generation : 0;
        in.modelCategories = entry.categories;
        in.modelFlags = entry.flags;
        in.facts = entry.facts;
        return in;
    }

    bool begin_shape(const J3DShapePacket* packet, u32 frame) noexcept {
        roll(frame);
        J3DModel* model = packet->getModel();
        J3DShape* shape = packet->getShape();
        ModelEntry& entry = drawn_model(model);
        if (entry.factsFrame != frame) {
            const J3DModelData* data = model->getModelData();
            if (entry.data != data) {
                entry.data = data;
                entry.info = &known_data(data);
            }
            stamp_frame(entry, frame, model_facts(model, data), data->checkBBoardFlag());
        }
        // Ahead of the suppression test, since shadow images draw a model before the main pass
        declare_arrays(*entry.info, entry.facts, shape, frame);
        if (m_suppression > 0) {
            ++m_current.suppressedShapes;
            return false;
        }

        ShapeInputs in = base_inputs(entry);
        in.shapeIndex = shape->getIndex();
        if (!entry.info->materials.empty()) {
            if (J3DMaterial* material = shape->getMaterial(); material != nullptr) {
                in.material = material_tag_at(entry.info->materials, material->getIndex());
            }
            in.pendingTags = pending_tags_enabled();
        }
        // Only models with a billboard joint pay for the virtual call
        in.facts.billboard = entry.billboardJoints && is_billboard_shape(shape);

        const ShapeObjectHint hint = compose_shape_object(in);
        GXAuroraBeginObject(hint.id, hint.subId, hint.generation, hint.categories, hint.flags);
        note_open();

        entry.lastDrawFrame = frame;
        ++m_current.objects;
        if ((hint.flags & AURORA_OBJ_CPU_DEFORMED) != 0) {
            ++m_current.cpuDeformed;
        }
        if ((hint.flags & AURORA_OBJ_BILLBOARD) != 0) {
            ++m_current.billboards;
        }
        if ((hint.categories & AURORA_CAT_STATIC) != 0) {
            ++m_current.staticObjects;
            if (entry.roomNo < 0) {
                ++m_current.nonRoomStatics;
            }
        }
        return true;
    }

    void push_suppression() noexcept { ++m_suppression; }

    void pop_suppression() noexcept {
        if (m_suppression == 0) {
            if (should_warn(Warning::SuppressionUnderflow)) {
                RegistryLog.warn("render registry: object suppression popped below zero");
            }
            return;
        }
        --m_suppression;
    }

    void note_open() noexcept {
        if (m_depth > 0) {
            ++m_current.nested;
        }
        ++m_depth;
        ++m_totalBegins;
    }

    void note_close() noexcept {
        if (m_depth > 0) {
            --m_depth;
        }
    }

    void roll(u32 frame) noexcept {
        if (frame == m_frame) {
            return;
        }
        if (m_depth != 0) {
            ++m_leaked;
            if (should_warn(Warning::Leak)) {
                RegistryLog.warn("render registry: {} objects still open at the end of frame {}",
                    m_depth, m_frame);
            }
            m_depth = 0;
        }
        if (array_log(m_frame)) {
            RegistryLog.info("render.objects log: frame {} objects {} suppressed {} nested {} "
                             "cpu_deformed {} billboard {} arrays declared {} released {}",
                m_current.frame, m_current.objects, m_current.suppressedShapes, m_current.nested,
                m_current.cpuDeformed, m_current.billboards, m_current.declaredArrays,
                m_current.releasedArrays);
        }
        m_last = m_current;
        m_current = RegistryStats{.frame = frame};
        m_frame = frame;
        m_totalBeginsAtRoll = m_totalBegins;
    }

    void set_log_until(u32 frame) noexcept { m_logUntil = frame; }
    u32 log_until() const noexcept { return m_logUntil; }
    const RegistryStats& current() const noexcept { return m_current; }
    const RegistryStats& last() const noexcept { return m_last; }
    u32 frame() const noexcept { return m_frame; }
    u32 depth() const noexcept { return m_depth; }
    u32 leaked() const noexcept { return m_leaked; }
    u64 total_begins() const noexcept { return m_totalBegins; }
    u64 total_begins_at_roll() const noexcept { return m_totalBeginsAtRoll; }
    const std::deque<ModelEntry>& models() const noexcept { return m_models; }
    const std::deque<DataInfo>& data() const noexcept { return m_data; }

    RegistrySnapshot snapshot() const noexcept {
        return {m_nextSerial, m_nextGeneration, m_models.size(), m_data.size(), m_logUntil,
            m_totalBegins};
    }

    // Entries are never erased and a reused address is reset in place, so a cached slot stays valid
    ModelEntry& drawn_model(const J3DModel* model) {
        const auto key = reinterpret_cast<uintptr_t>(model);
        auto& slot = m_drawCache[((key >> 4) ^ (key >> 12)) & (kDrawCacheSize - 1)];
        if (slot.first != model) {
            slot = {model, &known_model(model)};
        }
        return *slot.second;
    }

    void check_thread() {
        const std::thread::id id = std::this_thread::get_id();
        if (m_owner == std::thread::id{}) {
            m_owner = id;
        } else if (id != m_owner && should_warn(Warning::Thread)) {
            RegistryLog.warn("render registry: called off the main thread; tags may race");
        }
    }

    void track_arrays(const J3DModelData* data, std::vector<ImmutableArray> arrays,
        const char* archive, const JKRHeap* heap) {
        DataInfo& info = known_data(data);
        info.arrays = std::move(arrays);
        info.verified = false;
        if (info.arrays.empty() || heap == nullptr) {
            info.arrays.clear();
            return;
        }
        info.heap = heap;
        info.arrayArchive = archive != nullptr ? archive : "";
        m_heapData[heap].push_back(m_dataIndex.find(data)->second);
    }

    bool array_log(u32 frame) const noexcept {
        return !m_quiet && frame != kNoFrame && m_logUntil != 0 && frame <= m_logUntil;
    }

    bool take_declaration(DataInfo& info, u32 frame) noexcept {
        if (info.arrays.empty() || info.declaredFrame == frame) {
            return false;
        }
        roll(frame);
        info.declaredFrame = frame;
        info.declared = true;
        const auto count = static_cast<u32>(info.arrays.size());
        m_current.declaredArrays += count;
        m_declares.add(frame, count);
        return true;
    }

    ArrayRelease take_release(DataInfo& info, u32 frame) {
        ArrayRelease release;
        if (info.declared) {
            // Count a release from logic in the frame whose FIFO carries it
            roll(frame);
            release = {info.generation, std::move(info.arrays)};
            const auto count = static_cast<u32>(release.arrays.size());
            m_current.releasedArrays += count;
            m_releases.add(frame, count);
            if (array_log(frame)) {
                RegistryLog.info("render.arrays: frame {} release {} generation {} arrays {} "
                                 "first 0x{:x}",
                    frame, info.arrayArchive, release.generation, count,
                    count != 0 ? reinterpret_cast<uintptr_t>(release.arrays.front().data) : 0);
            }
        }
        // The data never declares again once its arrays were taken
        info.arrays.clear();
        info.heap = nullptr;
        info.declared = false;
        info.declaredFrame = kNoFrame;
        return release;
    }

    ArrayRelease release_data(const J3DModelData* data, u32 frame) {
        const auto it = m_dataIndex.find(data);
        if (it == m_dataIndex.end() || !m_data[it->second].declared) {
            return {};
        }
        return take_release(m_data[it->second], frame);
    }

    std::vector<ArrayRelease> release_heap(const JKRHeap* heap, u32 frame) {
        std::vector<ArrayRelease> releases;
        const auto it = m_heapData.find(heap);
        if (it == m_heapData.end()) {
            return releases;
        }
        const std::vector<u32> indices = std::move(it->second);
        m_heapData.erase(it);
        for (const u32 index : indices) {
            DataInfo& info = m_data[index];
            if (info.heap != heap) {
                continue;
            }
            ArrayRelease release = take_release(info, frame);
            if (!release.arrays.empty()) {
                releases.push_back(std::move(release));
            }
        }
        return releases;
    }

    void declare_arrays(DataInfo& info, const ShapeFacts& facts, J3DShape* shape, u32 frame) {
        if (info.arrays.empty()) {
            return;
        }
        // A deform setup flag can alias model-data arrays, so CPU deformers stop declaring for good
        if (facts.skinPosCpu || facts.deformData) {
            send_release(take_release(info, frame));
            return;
        }
        if (info.declaredFrame == frame) {
            return;
        }
        if (!info.verified) {
            info.verified = true;
            ++m_vcdChecks;
            if (!std::ranges::all_of(info.arrays,
                    [shape](const ImmutableArray& a) { return vcd_declares(shape, a); }))
            {
                ++m_vcdMisses;
                info.arrays.clear();
                return;
            }
            if (array_log(frame)) {
                RegistryLog.info("render.arrays: frame {} declare {} generation {} arrays {} "
                                 "first 0x{:x}",
                    frame, info.arrayArchive, info.generation, info.arrays.size(),
                    reinterpret_cast<uintptr_t>(info.arrays.front().data));
            }
        }
        if (take_declaration(info, frame)) {
            send_declaration(info);
        }
    }

    const SentCount& declares() const noexcept { return m_declares; }
    const SentCount& releases() const noexcept { return m_releases; }
    u32 vcd_checks() const noexcept { return m_vcdChecks; }
    u32 vcd_misses() const noexcept { return m_vcdMisses; }

private:
    ModelEntry& model_slot(const J3DModel* model) {
        const auto [it, inserted] =
            m_modelIndex.try_emplace(model, static_cast<u32>(m_models.size()));
        if (inserted) {
            m_models.emplace_back();
        }
        return m_models[it->second];
    }

    DataInfo& data_slot(const J3DModelData* data) {
        const auto [it, inserted] = m_dataIndex.try_emplace(data, static_cast<u32>(m_data.size()));
        if (inserted) {
            m_data.emplace_back();
        }
        return m_data[it->second];
    }

    ModelEntry& known_model(const J3DModel* model) {
        if (ModelEntry* entry = find_model(model); entry != nullptr) {
            return *entry;
        }
        if (should_warn(Warning::UnknownModel)) {
            RegistryLog.warn("render registry: saw a model before its initialization");
        }
        return initialize_model(model);
    }

    u32 next_serial() {
        if (m_nextSerial == kSerialLimit) {
            m_nextSerial = 1;
            if (should_warn(Warning::SerialWrap)) {
                RegistryLog.warn("render registry: model serials wrapped");
            }
        }
        return m_nextSerial++;
    }

    u32 next_generation() {
        if (m_nextGeneration == kSerialLimit) {
            m_nextGeneration = 1;
            if (should_warn(Warning::SerialWrap)) {
                RegistryLog.warn("render registry: model data generations wrapped");
            }
        }
        return m_nextGeneration++;
    }

    bool should_warn(Warning warning) noexcept {
        return !m_quiet && !std::exchange(m_warned[static_cast<size_t>(warning)], true);
    }

    void log_review_name(const char* archive, const char* name) const {
        if (m_quiet || name == nullptr) {
            return;
        }
        if (matches_cloud_scroll(name) && name[5] == '1' && name[6] == '6') {
            RegistryLog.info("render tags: {} '{}' {}", archive, name,
                matches_ma_code(name, "16") ? "ma16Exact" : "xx16Scroll");
        }
        if (matches_ma_code(name, "04")) {
            RegistryLog.info(
                "render tags: {} '{}' MA04 K1 writer without scroll; not a cloud receiver", archive,
                name);
        }
        if (matches_ma_code(name, "14")) {
            RegistryLog.info("render tags: {} '{}' MA14 pending NO_AO review", archive, name);
        }
    }

    std::deque<ModelEntry> m_models;
    absl::flat_hash_map<const J3DModel*, u32> m_modelIndex;
    std::deque<DataInfo> m_data;
    absl::flat_hash_map<const J3DModelData*, u32> m_dataIndex;
    absl::flat_hash_map<const JKRHeap*, std::vector<u32>> m_heapData;
    SentCount m_declares;
    SentCount m_releases;
    u32 m_vcdChecks = 0;
    u32 m_vcdMisses = 0;
    u32 m_nextSerial = 1;
    u32 m_nextGeneration = 1;
    std::array<std::pair<const J3DModel*, ModelEntry*>, kDrawCacheSize> m_drawCache{};
    u32 m_suppression = 0;
    u32 m_depth = 0;
    u32 m_frame = kNoFrame;
    u32 m_logUntil = 0;
    u64 m_totalBegins = 0;
    u64 m_totalBeginsAtRoll = 0;
    u32 m_leaked = 0;
    RegistryStats m_current;
    RegistryStats m_last;
    std::thread::id m_owner;
    std::array<bool, static_cast<size_t>(Warning::Count)> m_warned{};
    bool m_quiet;
};

// A function-local registry is ready for models constructed during static initialization
Registry& live() noexcept {
    // Never destroyed, since static destructors free archives after function-local statics die
    static Registry& registry = *new Registry;
    return registry;
}

const J3DModel* fake_model(uintptr_t address) noexcept {
    return reinterpret_cast<const J3DModel*>(address);
}

const J3DModelData* fake_data(uintptr_t address) noexcept {
    return reinterpret_cast<const J3DModelData*>(address);
}

const JKRHeap* fake_heap(uintptr_t address) noexcept {
    return reinterpret_cast<const JKRHeap*>(address);
}

const void* fake_array(uintptr_t address) noexcept {
    return reinterpret_cast<const void*>(address);
}

void append_problem(std::string& problems, std::string_view problem) {
    if (!problems.empty()) {
        problems += ", ";
    }
    problems += problem;
}

std::string test_serials_unique() {
    Registry registry(true);
    const u32 a = registry.initialize_model(fake_model(0x10)).serial;
    const u32 b = registry.initialize_model(fake_model(0x20)).serial;
    const u32 c = registry.initialize_model(fake_model(0x30)).serial;
    if (a == 0 || b == 0 || c == 0 || a == b || b == c || a == c) {
        return fmt::format("serials {} {} {}", a, b, c);
    }
    return {};
}

std::string test_reinit_resets() {
    Registry registry(true);
    const J3DModel* model = fake_model(0x40);
    const u32 first = registry.initialize_model(model).serial;
    registry.declare_room(model, 3, 1);
    const ModelEntry& entry = registry.initialize_model(model);
    if (entry.serial == first || entry.categories != AURORA_CAT_DYNAMIC ||
        (entry.flags & AURORA_OBJ_IMMOBILE) != 0 || entry.roomNo != -1)
    {
        return fmt::format("serial {} -> {}, categories {}, flags {}, room {}", first, entry.serial,
            category_names(entry.categories), entry.flags, entry.roomNo);
    }
    return {};
}

std::string test_room_static() {
    Registry registry(true);
    const J3DModel* model = fake_model(0x50);
    registry.initialize_model(model);
    registry.declare_room(model, 2, 4);
    const ModelEntry* entry = registry.find_model(model);
    if (entry == nullptr || entry->categories != (AURORA_CAT_STATIC | AURORA_CAT_TERRAIN) ||
        entry->flags != AURORA_OBJ_IMMOBILE || entry->roomNo != 2 || entry->part != 4)
    {
        return "room model is not STATIC|TERRAIN, IMMOBILE with its room and part";
    }
    return {};
}

std::string test_declare_rejects_static() {
    Registry registry(true);
    const J3DModel* model = fake_model(0x60);
    registry.initialize_model(model);
    registry.declare(model, AURORA_CAT_STATIC | AURORA_CAT_PLAYER, 0);
    const ModelEntry* entry = registry.find_model(model);
    if (entry == nullptr || entry->categories != AURORA_CAT_PLAYER) {
        return fmt::format("declared categories {}",
            entry == nullptr ? "missing" : category_names(entry->categories));
    }
    return {};
}

std::string test_generation() {
    Registry registry(true);
    const J3DModelData* data = fake_data(0x70);
    const u32 first = registry.create_data(data).generation;
    const u32 second = registry.create_data(data).generation;
    if (first == 0 || second <= first || registry.generation_of(data) != second ||
        registry.data().size() != 1)
    {
        return fmt::format(
            "generations {} then {}, lookup {}", first, second, registry.generation_of(data));
    }
    return {};
}

std::string test_ids() {
    std::string problems;
    constexpr std::array kinds{
        ObjectKind::J3dModel, ObjectKind::RealShadow, ObjectKind::SimpleShadow};
    for (const u32 value : {0u, 1u, 0x12345678u, 0xFFFFFFFFu}) {
        for (size_t i = 0; i < kinds.size(); ++i) {
            const u64 id = make_object_id(kinds[i], value);
            if (((id >> 32) & 0xFF) != static_cast<u64>(kinds[i]) || (id >> 40) != 0 ||
                static_cast<u32>(id) != value)
            {
                append_problem(problems, fmt::format("id 0x{:016x} has the wrong layout", id));
            }
            for (size_t j = 0; j < i; ++j) {
                if (id == make_object_id(kinds[j], value)) {
                    append_problem(problems, fmt::format("kinds collide for value {}", value));
                }
            }
        }
    }
    return problems;
}

std::string test_stable_entries() {
    Registry registry(true);
    const J3DModel* model = fake_model(0x80);
    const u32 serial = registry.initialize_model(model).serial;
    const ModelEntry* cached = registry.find_model(model);
    if (&registry.drawn_model(model) != cached) {
        return "the draw cache returns another entry";
    }
    for (uintptr_t i = 0; i < 20000; ++i) {
        const J3DModel* other = fake_model(0x100000 + i * 0x10);
        registry.initialize_model(other);
        registry.drawn_model(other);
    }
    if (registry.find_model(model) != cached || cached->serial != serial ||
        &registry.drawn_model(model) != cached)
    {
        return "a cached entry moved or changed after 20000 inserts";
    }
    const u32 reused = registry.initialize_model(model).serial;
    if (registry.drawn_model(model).serial != reused || reused == serial) {
        return "the draw cache misses a reinitialized model";
    }
    return {};
}

std::string test_teleport() {
    std::string problems;
    Registry registry(true);
    const J3DModel* model = fake_model(0x90);
    registry.initialize_model(model);
    if (!registry.mark_teleport(model, 7)) {
        append_problem(problems, "a known model is not marked");
    }
    ModelEntry& entry = *registry.find_model(model);
    registry.stamp_frame(entry, 7, {}, false);
    if ((compose_shape_object(registry.base_inputs(entry)).flags & AURORA_OBJ_TELEPORT) == 0) {
        append_problem(problems, "no TELEPORT in the marked frame");
    }
    registry.stamp_frame(entry, 8, {}, false);
    if ((compose_shape_object(registry.base_inputs(entry)).flags & AURORA_OBJ_TELEPORT) != 0) {
        append_problem(problems, "TELEPORT in the next frame");
    }
    registry.stamp_frame(entry, 9, {}, false);
    registry.mark_teleport(model, 9);
    if (entry.factsFrame != kNoFrame) {
        append_problem(problems, "a mark after the frame's stamp is not restamped");
    }
    const size_t models = registry.models().size();
    if (registry.mark_teleport(fake_model(0xA0), 7) || registry.models().size() != models) {
        append_problem(problems, "an unknown model is marked or inserted");
    }
    return problems;
}

std::string test_compose_tags() {
    std::string problems;
    Registry registry(true);
    const J3DModel* model = fake_model(0xB0);
    registry.initialize_model(model);
    registry.declare_room(model, 0, 0);
    ShapeInputs in = registry.base_inputs(*registry.find_model(model));
    const MaterialTag ma00 = material_tag_for_name("abcMA00");
    in.material = &ma00;
    const ShapeObjectHint hint = compose_shape_object(in);
    if (hint.categories != (AURORA_CAT_STATIC | AURORA_CAT_TERRAIN | AURORA_CAT_CLOUD_RECEIVER) ||
        hint.flags != AURORA_OBJ_IMMOBILE)
    {
        append_problem(problems, fmt::format("room MA00 gives {} flags {}",
                                     category_names(hint.categories), hint.flags));
    }
    const MaterialTag ma14 = material_tag_for_name("abcMA14");
    in.material = &ma14;
    if ((compose_shape_object(in).categories & AURORA_CAT_NO_AO) != 0) {
        append_problem(problems, "pending NO_AO applied while pending tags are off");
    }
    in.pendingTags = true;
    if ((compose_shape_object(in).categories & AURORA_CAT_NO_AO) == 0) {
        append_problem(problems, "pending NO_AO missing while pending tags are on");
    }
    return problems;
}

std::string test_compose_water() {
    ShapeInputs in;
    in.modelCategories = AURORA_CAT_STATIC | AURORA_CAT_TERRAIN;
    const MaterialTag water = material_tag_for_name("abcMA03x");
    in.material = &water;
    const ShapeObjectHint hint = compose_shape_object(in);
    if (hint.categories != (AURORA_CAT_STATIC | AURORA_CAT_WATER | AURORA_CAT_NO_PBR)) {
        return fmt::format("room water gives {}", category_names(hint.categories));
    }
    return {};
}

std::string test_compose_flags() {
    std::string problems;
    const auto flags_for = [](auto&& set) {
        ShapeInputs in;
        set(in);
        return compose_shape_object(in).flags;
    };
    const std::array<std::tuple<std::string_view, u32, u32>, 7> cases{{
        {"skinPosCpu", flags_for([](ShapeInputs& in) { in.facts.skinPosCpu = true; }),
            kCpuDeformed},
        {"deformData", flags_for([](ShapeInputs& in) { in.facts.deformData = true; }),
            kCpuDeformed},
        {"localPositions", flags_for([](ShapeInputs& in) { in.facts.localPositions = true; }),
            kCpuDeformed},
        {"billboard", flags_for([](ShapeInputs& in) { in.facts.billboard = true; }),
            AURORA_OBJ_BILLBOARD},
        {"teleport", flags_for([](ShapeInputs& in) { in.facts.teleport = true; }),
            AURORA_OBJ_TELEPORT},
        {"fast skin only", flags_for([](ShapeInputs&) {}), 0},
        {"immobile", flags_for([](ShapeInputs& in) { in.modelFlags = AURORA_OBJ_IMMOBILE; }),
            AURORA_OBJ_IMMOBILE},
    }};
    for (const auto& [name, got, expected] : cases) {
        if (got != expected) {
            append_problem(problems, fmt::format("{} gives flags {}", name, got));
        }
    }
    ShapeInputs in;
    in.serial = 9;
    in.shapeIndex = 3;
    in.generation = 5;
    const ShapeObjectHint hint = compose_shape_object(in);
    if (hint.id != make_object_id(ObjectKind::J3dModel, 9) || hint.subId != 3 ||
        hint.generation != 5)
    {
        append_problem(problems, "identity fields differ");
    }
    return problems;
}

std::string test_material_bound() {
    std::string problems;
    const std::vector<MaterialTag> tags(3, material_tag_for_name("abcMA03x"));
    if (material_tag_at(tags, 3) != nullptr || material_tag_at(tags, 2) != &tags[2] ||
        material_tag_at({}, 0) != nullptr)
    {
        append_problem(problems, "material index bound wrong");
    }
    ShapeInputs in;
    in.modelCategories = AURORA_CAT_DYNAMIC;
    in.material = material_tag_at(tags, 3);
    if (compose_shape_object(in).categories != AURORA_CAT_DYNAMIC) {
        append_problem(problems, "an out-of-range material adds categories");
    }
    return problems;
}

std::string test_nesting() {
    std::string problems;
    Registry registry(true);
    registry.roll(1);
    registry.note_open();
    registry.note_open();
    registry.note_close();
    registry.note_close();
    if (registry.current().nested != 1 || registry.depth() != 0) {
        append_problem(problems,
            fmt::format("nested {} depth {}", registry.current().nested, registry.depth()));
    }
    registry.note_open();
    registry.roll(2);
    if (registry.leaked() != 1 || registry.depth() != 0 || registry.last().nested != 1) {
        append_problem(problems,
            fmt::format("leaked {} depth {} after the roll", registry.leaked(), registry.depth()));
    }
    return problems;
}

std::string test_aurora_agrees() {
    std::string problems;
    const Registry& registry = live();
    const auto begin = aurora::render::hint_counters(GX_AURORA_RENDER_OBJECT_BEGIN);
    const auto end = aurora::render::hint_counters(GX_AURORA_RENDER_OBJECT_END);
    // Objects of the frame still being recorded have not reached the decoder yet
    const u64 sent = registry.frame() == aurora::gfx::current_frame() ?
                         registry.total_begins_at_roll() :
                         registry.total_begins();
    if (begin.decoded != sent) {
        append_problem(problems,
            fmt::format("aurora decoded {} object begins, dusk sent {}", begin.decoded, sent));
    }
    if (end.decoded != begin.decoded) {
        append_problem(problems,
            fmt::format("aurora decoded {} object begins, {} ends", begin.decoded, end.decoded));
    }
    if (begin.malformed != 0 || begin.late != 0 || end.malformed != 0 || end.late != 0) {
        append_problem(problems, fmt::format("malformed {} {} late {} {}", begin.malformed,
                                     end.malformed, begin.late, end.late));
    }
    return problems;
}

std::string test_immutable_select() {
    std::string problems;
    auto slots = empty_slots();
    slots[GX_VA_POS - GX_VA_POS] = {GX_VA_POS, fake_array(0x1000), 120};
    slots[GX_VA_NRM - GX_VA_POS] = {GX_VA_NRM, fake_array(0x2000), 60};
    slots[GX_VA_TEX0 - GX_VA_POS] = {GX_VA_TEX0, fake_array(0x3000), 32};
    const std::vector<ImmutableArray> all = select_immutable(slots, true);
    if (all.size() != 3 || all[0] != slots[0] || all[2] != slots[GX_VA_TEX0 - GX_VA_POS]) {
        append_problem(problems, fmt::format("{} arrays from three filled slots", all.size()));
    }
    const std::vector<ImmutableArray> skinned = select_immutable(slots, false);
    if (skinned.size() != 1 || skinned[0].attr != GX_VA_TEX0) {
        append_problem(problems, "POS or NRM kept under in-place skinning");
    }
    auto repeated = slots;
    repeated[GX_VA_TEX1 - GX_VA_POS] = {GX_VA_TEX1, fake_array(0x3000), 32};
    if (!select_immutable(repeated, true).empty()) {
        append_problem(problems, "a repeated pointer is declared");
    }
    auto sizeless = slots;
    sizeless[GX_VA_CLR0 - GX_VA_POS] = {GX_VA_CLR0, fake_array(0x4000), 0};
    if (!select_immutable(sizeless, true).empty()) {
        append_problem(problems, "a slot without bytes is declared");
    }

    const std::array<GXVtxDescList, 3> direct{
        {{GX_VA_POS, GX_INDEX16}, {GX_VA_CLR0, GX_DIRECT}, {GX_VA_NULL, GX_NONE}}};
    if (reads_missing_slot(direct.data(), slots)) {
        append_problem(problems, "a direct attribute over an empty slot counts as missing");
    }
    const std::array<GXVtxDescList, 2> indexed{{{GX_VA_TEX1, GX_INDEX8}, {GX_VA_NULL, GX_NONE}}};
    if (!reads_missing_slot(indexed.data(), slots)) {
        append_problem(problems, "an indexed empty slot is not missing");
    }
    const std::array<GXVtxDescList, 2> nbt{{{GX_VA_NBT, GX_INDEX16}, {GX_VA_NULL, GX_NONE}}};
    if (!reads_missing_slot(nbt.data(), slots) || !reads_missing_slot(nullptr, slots)) {
        append_problem(problems, "an NBT shape or a missing descriptor list is accepted");
    }
    return problems;
}

std::string test_array_lifecycle() {
    std::string problems;
    Registry registry(true);
    const JKRHeap* heapA = fake_heap(0x10000);
    const JKRHeap* heapB = fake_heap(0x20000);
    const J3DModelData* first = fake_data(0x210);
    const J3DModelData* idle = fake_data(0x220);
    const J3DModelData* reused = fake_data(0x230);
    const J3DModelData* rebuilt = fake_data(0x240);
    const std::vector<ImmutableArray> arrays{
        {GX_VA_POS, fake_array(0x5000), 96}, {GX_VA_TEX0, fake_array(0x6000), 48}};
    const std::vector<ImmutableArray> single{{GX_VA_POS, fake_array(0x7000), 24}};
    for (const J3DModelData* data : {first, idle, reused, rebuilt}) {
        registry.create_data(data);
    }
    registry.track_arrays(first, arrays, "arcA", heapA);
    registry.track_arrays(idle, single, "arcA", heapA);
    registry.track_arrays(reused, single, "arcB", heapB);
    registry.track_arrays(rebuilt, single, "arcB", heapB);
    DataInfo& a = registry.known_data(first);
    DataInfo& b = registry.known_data(idle);
    DataInfo& c = registry.known_data(reused);

    if (!registry.take_declaration(a, 1) || registry.take_declaration(a, 1) ||
        !registry.take_declaration(c, 1) || registry.current().declaredArrays != 3)
    {
        append_problem(problems, "a frame declares other than once per data");
    }
    if (registry.declares().settled(1) != 0 || registry.declares().total != 3) {
        append_problem(problems, "the frame's own declarations count as settled");
    }
    if (!registry.take_declaration(a, 2) || registry.declares().settled(2) != 3) {
        append_problem(problems, "the next frame does not declare again");
    }

    const std::vector<ArrayRelease> released = registry.release_heap(heapA, 2);
    if (released.size() != 1 || released[0].generation != a.generation ||
        released[0].arrays != arrays || registry.current().releasedArrays != 2)
    {
        append_problem(problems, "the heap release does not return the declared arrays");
    }
    if (!b.arrays.empty() || !a.arrays.empty() || a.heap != nullptr) {
        append_problem(problems, "released data keeps its arrays");
    }
    if (!registry.release_heap(heapA, 3).empty() || registry.take_declaration(a, 3)) {
        append_problem(problems, "a destroyed heap releases again or its data declares again");
    }

    const u32 oldGeneration = c.generation;
    const ArrayRelease reuse = registry.release_data(reused, 3);
    registry.create_data(reused);
    if (reuse.generation != oldGeneration || reuse.arrays != single ||
        registry.known_data(reused).generation <= oldGeneration)
    {
        append_problem(problems, "an address reuse does not release the old generation");
    }

    registry.take_declaration(registry.known_data(rebuilt), 3);
    registry.create_data(rebuilt);
    const u32 frame = registry.frame();
    if (!registry.release_heap(heapB, 4).empty() || registry.frame() != frame) {
        append_problem(problems, "a rebuilt data is still released with its old heap");
    }
    if (!registry.release_data(fake_data(0x990), 9).arrays.empty() || registry.frame() != frame) {
        append_problem(problems, "unknown data releases or rolls the frame");
    }

    const J3DModelData* heapless = fake_data(0x250);
    registry.create_data(heapless);
    registry.track_arrays(heapless, arrays, "arcC", nullptr);
    if (!registry.known_data(heapless).arrays.empty()) {
        append_problem(problems, "data without a heap keeps arrays it could never release");
    }
    return problems;
}

std::string test_array_hints_agree() {
    std::string problems;
    const Registry& registry = live();
    const u32 now = aurora::gfx::current_frame();
    const auto agree = [&](u16 id, const SentCount& sent, std::string_view name) {
        const auto counters = aurora::render::hint_counters(id);
        const u64 seen = counters.decoded + counters.skipped;
        if (seen < sent.settled(now) || seen > sent.total) {
            append_problem(
                problems, fmt::format("aurora saw {} {} hints, dusk sent {} ({} settled)", seen,
                              name, sent.total, sent.settled(now)));
        }
        if (counters.malformed != 0 || counters.late != 0) {
            append_problem(problems,
                fmt::format("{} malformed {} late {}", name, counters.malformed, counters.late));
        }
    };
    agree(GX_AURORA_SET_ARRAY_IMMUTABLE, registry.declares(), "set");
    agree(GX_AURORA_RELEASE_ARRAY_IMMUTABLE, registry.releases(), "release");
    if (registry.vcd_misses() != 0) {
        append_problem(problems, fmt::format("{} of {} model data failed the array base check",
                                     registry.vcd_misses(), registry.vcd_checks()));
    }
    return problems;
}

}  // namespace

void on_model_initialized(const J3DModel* model) noexcept {
    Registry& registry = live();
    registry.check_thread();
    registry.initialize_model(model);
}

void on_model_data_created(const J3DModelData* data) noexcept {
    Registry& registry = live();
    registry.check_thread();
    // A reused address means the old data died without its heap release
    send_release(registry.release_data(data, aurora::gfx::current_frame()));
    registry.create_data(data);
}

void on_model_loaded(const J3DModelData* data, const char* archive, const JKRHeap* heap) noexcept {
    Registry& registry = live();
    registry.check_thread();
    registry.load_data(data, archive, heap);
}

void on_resource_heap_destroyed(const JKRHeap* heap) noexcept {
    // aurora has shut down when static destructors free the remaining archives
    if (dusk::IsShuttingDown) {
        return;
    }
    Registry& registry = live();
    registry.check_thread();
    for (const ArrayRelease& release : registry.release_heap(heap, aurora::gfx::current_frame())) {
        send_release(release);
    }
}

void declare_room_model(const J3DModel* model, int roomNo, int part) noexcept {
    Registry& registry = live();
    registry.check_thread();
    registry.declare_room(model, roomNo, part);
}

void declare_model(const J3DModel* model, u32 categories, u32 flags) noexcept {
    Registry& registry = live();
    registry.check_thread();
    registry.declare(model, categories, flags);
}

void mark_model_teleport(const J3DModel* model) noexcept {
    Registry& registry = live();
    registry.check_thread();
    registry.mark_teleport(model, aurora::gfx::current_frame());
}

u32 model_serial(const J3DModel* model) noexcept {
    return live().serial_of(model);
}

u32 data_generation(const J3DModelData* data) noexcept {
    return live().generation_of(data);
}

ShapeObjectHint compose_shape_object(const ShapeInputs& in) noexcept {
    u32 categories = in.modelCategories;
    if (in.material != nullptr) {
        categories |= in.material->categories;
        if (in.pendingTags) {
            categories |= in.material->pendingCategories;
        }
    }
    // Water is never terrain for the passes that treat terrain specially
    if ((categories & AURORA_CAT_WATER) != 0) {
        categories &= ~AURORA_CAT_TERRAIN;
    }
    u32 flags = in.modelFlags;
    if (in.facts.skinPosCpu || in.facts.deformData || in.facts.localPositions) {
        flags |= AURORA_OBJ_CPU_DEFORMED;
    }
    if (in.facts.billboard) {
        flags |= AURORA_OBJ_BILLBOARD;
    }
    if (in.facts.teleport) {
        flags |= AURORA_OBJ_TELEPORT;
    }
    return {make_object_id(ObjectKind::J3dModel, in.serial), in.shapeIndex, in.generation,
        categories, flags};
}

void push_object_suppression() noexcept {
    live().push_suppression();
}

void pop_object_suppression() noexcept {
    live().pop_suppression();
}

void note_object_open() noexcept {
    live().note_open();
}

void note_object_close() noexcept {
    live().note_close();
}

bool begin_shape(const J3DShapePacket* packet) noexcept {
    if (!hints_active()) {
        return false;
    }
    return live().begin_shape(packet, aurora::gfx::current_frame());
}

RegistryStats current_registry_stats() noexcept {
    Registry& registry = live();
    registry.roll(aurora::gfx::current_frame());
    return registry.current();
}

RegistryStats last_registry_stats() noexcept {
    return live().last();
}

void set_object_log(u32 frames) noexcept {
    live().set_log_until(aurora::gfx::current_frame() + frames);
}

u32 object_log_until() noexcept {
    return live().log_until();
}

std::vector<std::string> registry_report() {
    Registry& registry = live();
    registry.roll(aurora::gfx::current_frame());
    const RegistryStats& last = registry.last();
    const RegistryStats& current = registry.current();
    std::vector<std::string> lines;
    lines.push_back(fmt::format(
        "render.objects: frame {} objects {} static {} non-room static {} suppressed {} nested {}",
        last.frame, last.objects, last.staticObjects, last.nonRoomStatics, last.suppressedShapes,
        last.nested));
    lines.push_back(fmt::format("render.objects: current frame {} objects {} (in progress)",
        current.frame, current.objects));
    const auto tagged = std::ranges::count_if(
        registry.data(), [](const DataInfo& info) { return !info.materials.empty(); });
    lines.push_back(fmt::format("render.objects: registry models {} model data {} tagged {} "
                                "leaked {}",
        registry.models().size(), registry.data().size(), tagged, registry.leaked()));
    const auto immutable = std::ranges::count_if(
        registry.data(), [](const DataInfo& info) { return !info.arrays.empty(); });
    lines.push_back(
        fmt::format("render.objects: immutable data {} sent {} released {} vcd checks {} misses {}",
            immutable, registry.declares().total, registry.releases().total, registry.vcd_checks(),
            registry.vcd_misses()));

    std::vector<std::tuple<int, int, u64, u32>> rooms;
    for (const ModelEntry& entry : registry.models()) {
        if ((entry.categories & AURORA_CAT_STATIC) != 0 && entry.lastDrawFrame == last.frame &&
            entry.lastDrawFrame != kNoFrame)
        {
            rooms.emplace_back(entry.roomNo, entry.part,
                make_object_id(ObjectKind::J3dModel, entry.serial),
                entry.info != nullptr ? entry.info->generation : 0);
        }
    }
    std::ranges::sort(rooms);
    for (const auto& [room, part, id, generation] : rooms) {
        lines.push_back(fmt::format(
            "  room {} part {} id 0x{:016x} generation {}", room, part, id, generation));
    }
    return lines;
}

std::vector<std::string> material_tag_report() {
    std::vector<std::string> lines;
    for (const DataInfo& info : live().data()) {
        for (size_t i = 0; i < info.materials.size(); ++i) {
            const MaterialTag& tag = info.materials[i];
            if (tag.categories == 0 && tag.pendingCategories == 0 && tag.tevPolicy.mask == 0) {
                continue;
            }
            std::string line = fmt::format("  {} '{}' {}", info.archive,
                i < info.names.size() ? info.names[i] : std::string{},
                category_names(tag.categories));
            if (tag.tevPolicy.mask != 0) {
                line += fmt::format(" policy {}", tev_policy_text(tag.tevPolicy));
            }
            if (tag.pendingCategories != 0) {
                line += fmt::format(" pending {}", category_names(tag.pendingCategories));
            }
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

void registry_self_test(SelfTestReport& report) {
    const auto check = [&report](std::string_view name, const std::string& failure) {
        if (failure.empty()) {
            ++report.passed;
        } else {
            report.failed.push_back(fmt::format("{}: {}", name, failure));
        }
    };
    const RegistrySnapshot before = live().snapshot();
    check("registry.serials_unique", test_serials_unique());
    check("registry.reinit_resets", test_reinit_resets());
    check("registry.room_static", test_room_static());
    check("registry.declare_rejects_static", test_declare_rejects_static());
    check("registry.generation", test_generation());
    check("registry.ids", test_ids());
    check("registry.stable_entries", test_stable_entries());
    check("registry.teleport", test_teleport());
    check("registry.compose_tags", test_compose_tags());
    check("registry.compose_water", test_compose_water());
    check("registry.compose_flags", test_compose_flags());
    check("registry.material_bound", test_material_bound());
    check("registry.nesting", test_nesting());
    check("registry.immutable_select", test_immutable_select());
    check("registry.array_lifecycle", test_array_lifecycle());
    check("registry.live_untouched",
        live().snapshot() == before ? std::string{} : "the live registry changed during the suite");
    if (!hints_active()) {
        report.skipped.push_back("registry.aurora_agrees: hints are off or no painter yet");
        report.skipped.push_back("registry.array_hints_agree: hints are off or no painter yet");
        return;
    }
    check("registry.aurora_agrees", test_aurora_agrees());
    if (live().vcd_checks() == 0) {
        report.skipped.push_back("registry.array_hints_agree: no model data has declared arrays");
        return;
    }
    check("registry.array_hints_agree", test_array_hints_agree());
}

}  // namespace dusk::render
