#include "lantern/service.h"

#include "network_client.hpp"
#include "protocol.hpp"

#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/host.h>
#include <mods/svc/log.h>
#include <mods/svc/ui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

DEFINE_MOD();
IMPORT_SERVICE(HostService, svc_host);
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

namespace {

using lantern::protocol::CompatibilityReport;
using lantern::protocol::Delivery;
using lantern::protocol::FrameView;
using lantern::protocol::ManifestUpdate;
using lantern::protocol::MessageType;
using lantern::protocol::ModuleData;
using lantern::protocol::ModuleManifest;
using lantern::protocol::PeerManifest;

constexpr auto kDrainBudget = std::chrono::milliseconds(2);

struct Registration {
    ModContext* owner = nullptr;
    LanternModuleHandle handle = 0;
    std::string module_id;
    std::string owner_mod_id;
    std::string owner_mod_version;
    std::string display_version;
    uint16_t protocol_major = 1;
    uint16_t protocol_minor = 0;
    uint16_t minimum_peer_minor = 0;
    uint32_t flags = 0;
    std::string compatibility_tag;
    std::string distribution_id;
    LanternSessionFn on_session = nullptr;
    LanternPeerFn on_peer = nullptr;
    LanternManifestFn on_manifest = nullptr;
    LanternMessageFn on_message = nullptr;
    void* user_data = nullptr;
    uint32_t next_sequence = 1;

    ModuleManifest manifest() const {
        return {
            .module_id = module_id,
            .owner_mod_id = owner_mod_id,
            .owner_mod_version = owner_mod_version,
            .display_version = display_version,
            .protocol_major = protocol_major,
            .protocol_minor = protocol_minor,
            .minimum_peer_minor = minimum_peer_minor,
            .flags = flags,
            .compatibility_tag = compatibility_tag,
            .distribution_id = distribution_id,
        };
    }
};

struct PeerState {
    PeerManifest peer;
    CompatibilityReport compatibility;
    std::unordered_set<std::string> compatible_modules;
    std::unordered_map<std::string, uint32_t> last_unreliable_sequence;
};

class LanternCore {
public:
    ModResult initialize(ModError* error) {
        register_settings();
        register_ui();
        std::string network_error;
        if (!network_.start(network_error)) {
            return mods::set_error(error, MOD_ERROR, network_error.c_str());
        }
        if (svc_host->watch_mod_lifecycle(
                mod_ctx, &LanternCore::lifecycle_callback, this, &lifecycle_watch_) != MOD_OK)
        {
            return mods::set_error(error, MOD_ERROR, "Lantern could not watch mod lifecycle");
        }
        svc_log->info(mod_ctx, "Lantern Core initialized");
        return MOD_OK;
    }

    ModResult update(ModError*) {
        const auto deadline = std::chrono::steady_clock::now() + kDrainBudget;
        lantern::net::ClientEvent event;
        while (std::chrono::steady_clock::now() < deadline && network_.poll(event)) {
            handle_network_event(event);
        }
        return MOD_OK;
    }

    ModResult shutdown(ModError*) {
        disconnect("Lantern Core unloaded");
        network_.stop();
        if (lifecycle_watch_ != 0) {
            svc_host->unwatch_mod_lifecycle(mod_ctx, lifecycle_watch_);
            lifecycle_watch_ = 0;
        }
        registrations_.clear();
        registration_by_id_.clear();
        svc_log->info(mod_ctx, "Lantern Core unloaded");
        return MOD_OK;
    }

    ModResult register_module(
        ModContext* owner, const LanternModuleDesc* desc, LanternModuleHandle* out_handle) {
        if (owner == nullptr || desc == nullptr || out_handle == nullptr ||
            desc->struct_size < sizeof(LanternModuleDesc) || desc->module_id == nullptr ||
            desc->module_id[0] == '\0' || desc->protocol_major == 0)
        {
            return MOD_INVALID_ARGUMENT;
        }
        const std::string module_id = desc->module_id;
        if (module_id.size() > lantern::protocol::kMaxIdBytes ||
            registration_by_id_.contains(module_id))
        {
            return registration_by_id_.contains(module_id) ? MOD_CONFLICT : MOD_INVALID_ARGUMENT;
        }
        Registration registration;
        registration.owner = owner;
        registration.handle = next_registration_++;
        registration.module_id = module_id;
        registration.owner_mod_id = safe_string(svc_host->mod_id(owner));
        registration.owner_mod_version = safe_string(svc_host->mod_version(owner));
        registration.display_version = safe_string(desc->display_version);
        registration.protocol_major = desc->protocol_major;
        registration.protocol_minor = desc->protocol_minor;
        registration.minimum_peer_minor = desc->minimum_peer_minor;
        registration.flags = desc->flags;
        registration.compatibility_tag = safe_string(desc->compatibility_tag);
        registration.distribution_id = safe_string(desc->distribution_id);
        registration.on_session = desc->on_session;
        registration.on_peer = desc->on_peer;
        registration.on_manifest = desc->on_manifest;
        registration.on_message = desc->on_message;
        registration.user_data = desc->user_data;
        if (registration.owner_mod_id.empty() ||
            registration.owner_mod_version.size() > lantern::protocol::kMaxVersionBytes ||
            registration.display_version.size() > lantern::protocol::kMaxVersionBytes ||
            registration.compatibility_tag.size() > lantern::protocol::kMaxTagBytes ||
            registration.distribution_id.size() > lantern::protocol::kMaxIdBytes)
        {
            return MOD_INVALID_ARGUMENT;
        }

        const LanternModuleHandle handle = registration.handle;
        registration_by_id_[registration.module_id] = handle;
        registrations_.emplace(handle, std::move(registration));
        *out_handle = handle;
        renegotiate();
        return MOD_OK;
    }

    ModResult unregister_module(ModContext* owner, LanternModuleHandle handle) {
        const auto it = registrations_.find(handle);
        if (owner == nullptr || it == registrations_.end() || it->second.owner != owner) {
            return MOD_INVALID_ARGUMENT;
        }
        registration_by_id_.erase(it->second.module_id);
        registrations_.erase(it);
        renegotiate();
        return MOD_OK;
    }

    ModResult send(ModContext* owner, const LanternSendDesc* desc) {
        if (owner == nullptr || desc == nullptr || desc->struct_size < sizeof(LanternSendDesc) ||
            desc->data == nullptr || desc->size == 0 ||
            desc->size > lantern::protocol::kMaxModulePayloadBytes ||
            (desc->delivery != LANTERN_RELIABLE_ORDERED &&
                desc->delivery != LANTERN_UNRELIABLE_LATEST) ||
            state_ != LANTERN_CONNECTED)
        {
            return state_ == LANTERN_CONNECTED ? MOD_INVALID_ARGUMENT : MOD_UNAVAILABLE;
        }
        const auto registration_it = registrations_.find(desc->module);
        if (registration_it == registrations_.end() || registration_it->second.owner != owner) {
            return MOD_INVALID_ARGUMENT;
        }
        auto& registration = registration_it->second;
        if (desc->target_peer_id != 0) {
            const auto peer = peers_.find(desc->target_peer_id);
            if (peer == peers_.end() ||
                !peer->second.compatible_modules.contains(registration.module_id))
            {
                return MOD_UNAVAILABLE;
            }
        }

        ModuleData message;
        message.target_peer_id = desc->target_peer_id;
        message.delivery = desc->delivery == LANTERN_RELIABLE_ORDERED ? Delivery::ReliableOrdered :
                                                                        Delivery::UnreliableLatest;
        message.sequence = registration.next_sequence++;
        message.module_id = registration.module_id;
        const auto* bytes = static_cast<const uint8_t*>(desc->data);
        message.payload.assign(bytes, bytes + desc->size);
        std::vector<uint8_t> frame;
        std::string encode_error;
        if (!lantern::protocol::encode(message, frame, encode_error)) {
            return MOD_INVALID_ARGUMENT;
        }
        const bool reliable = desc->delivery == LANTERN_RELIABLE_ORDERED;
        const std::string latest_key =
            reliable ? std::string{} :
                       registration.module_id + ":" + std::to_string(desc->target_peer_id);
        return network_.send(std::move(frame), reliable, latest_key) ? MOD_OK : MOD_UNAVAILABLE;
    }

    LanternConnectionState connection_state() const { return state_; }
    LanternPeerId self_peer_id() const { return self_peer_id_; }
    size_t peer_count() const { return peers_.size(); }

    ModResult peer_at(size_t index, LanternPeerInfo* out) const {
        if (out == nullptr || out->struct_size < sizeof(LanternPeerInfo) || index >= peers_.size())
        {
            return MOD_INVALID_ARGUMENT;
        }
        auto it = peers_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        fill_peer_info(it->second, *out);
        return MOD_OK;
    }

    const char* room_code() const { return room_code_.c_str(); }
    const char* compatibility_report() const { return compatibility_report_.c_str(); }

    void create_room() { connect(true); }
    void join_room() { connect(false); }

    void disconnect(std::string reason = "Disconnected") {
        network_.disconnect(reason);
        clear_session(reason, LANTERN_SESSION_DISCONNECTED);
    }

private:
    static std::string safe_string(const char* value) { return value == nullptr ? "" : value; }

    static void lifecycle_callback(
        ModContext*, ModContext* subject, const char*, ModLifecycleEvent event, void* user_data) {
        if (event == MOD_LIFECYCLE_DETACHED) {
            static_cast<LanternCore*>(user_data)->detach_owner(subject);
        }
    }

    void detach_owner(ModContext* owner) {
        bool changed = false;
        for (auto it = registrations_.begin(); it != registrations_.end();) {
            if (it->second.owner == owner) {
                registration_by_id_.erase(it->second.module_id);
                it = registrations_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) {
            renegotiate();
        }
    }

    std::vector<ModuleManifest> manifest() const {
        std::vector<ModuleManifest> result;
        result.reserve(registrations_.size());
        for (const auto& [handle, registration] : registrations_) {
            (void)handle;
            result.push_back(registration.manifest());
        }
        std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.module_id < right.module_id; });
        return result;
    }

    void connect(bool create) {
        if (state_ != LANTERN_DISCONNECTED) {
            disconnect("Starting a new connection");
        }
        lantern::protocol::Hello hello;
        hello.create_room = create;
        hello.room_code = create ? "" : config_string(room_code_config_, "");
        hello.peer.display_name = config_string(display_name_config_, "Player");
        hello.peer.color_rgb = static_cast<uint32_t>(config_int(color_config_, 0x77C8FF));
        hello.peer.modules = manifest();
        std::vector<uint8_t> frame;
        std::string error;
        if (!lantern::protocol::encode(hello, frame, error)) {
            compatibility_report_ = error;
            return;
        }
        state_ = LANTERN_CONNECTING;
        compatibility_report_ = "Connecting...";
        notify_session(LANTERN_SESSION_CONNECTING, "Connecting");
        network_.connect(config_string(server_config_, "127.0.0.1:43384"), std::move(frame));
    }

    void renegotiate() {
        recompute_compatibility();
        if (state_ != LANTERN_CONNECTED) {
            return;
        }
        ManifestUpdate update;
        update.modules = manifest();
        std::vector<uint8_t> frame;
        std::string error;
        if (lantern::protocol::encode(update, frame, error)) {
            network_.send(std::move(frame), true);
            notify_session(LANTERN_SESSION_COMPATIBILITY_CHANGED, "Local manifest changed");
        }
    }

    void handle_network_event(const lantern::net::ClientEvent& event) {
        switch (event.type) {
        case lantern::net::ClientEventType::TransportConnected:
            state_ = LANTERN_NEGOTIATING;
            compatibility_report_ = "Negotiating module manifests...";
            break;
        case lantern::net::ClientEventType::TransportDisconnected:
            clear_session(event.detail.empty() ? "Connection closed" : event.detail,
                LANTERN_SESSION_DISCONNECTED);
            break;
        case lantern::net::ClientEventType::Error:
            clear_session(event.detail, LANTERN_SESSION_DISCONNECTED);
            svc_log->error(mod_ctx, event.detail.c_str());
            break;
        case lantern::net::ClientEventType::Frame:
            handle_frame(event.frame);
            break;
        }
    }

    void handle_frame(const std::vector<uint8_t>& bytes) {
        FrameView frame;
        std::string error;
        if (!lantern::protocol::decode_frame(bytes, frame, error)) {
            svc_log->warn(mod_ctx, ("Lantern dropped malformed frame: " + error).c_str());
            return;
        }
        switch (frame.type) {
        case MessageType::Welcome: {
            lantern::protocol::Welcome welcome;
            if (!lantern::protocol::decode(frame, welcome, error))
                break;
            self_peer_id_ = welcome.self_peer_id;
            room_code_ = std::move(welcome.room_code);
            if (room_code_config_ != 0) {
                svc_config->set_string(mod_ctx, room_code_config_, room_code_.c_str());
            }
            peers_.clear();
            for (auto& peer : welcome.peers) {
                if (peer.peer_id != self_peer_id_)
                    add_peer(std::move(peer), false);
            }
            state_ = LANTERN_CONNECTED;
            recompute_compatibility();
            notify_session(LANTERN_SESSION_CONNECTED, room_code_.c_str());
            break;
        }
        case MessageType::Reject: {
            lantern::protocol::Reject reject;
            if (!lantern::protocol::decode(frame, reject, error))
                break;
            compatibility_report_ = reject.reason;
            notify_session(LANTERN_SESSION_REJECTED, reject.reason.c_str());
            network_.disconnect("Join rejected");
            state_ = LANTERN_DISCONNECTED;
            break;
        }
        case MessageType::PeerJoined: {
            PeerManifest peer;
            if (!lantern::protocol::decode_peer_joined(frame, peer, error))
                break;
            add_peer(std::move(peer), true);
            break;
        }
        case MessageType::PeerLeft: {
            lantern::protocol::PeerLeft left;
            if (!lantern::protocol::decode(frame, left, error))
                break;
            remove_peer(left.peer_id);
            break;
        }
        case MessageType::ManifestUpdate: {
            ManifestUpdate update;
            if (!lantern::protocol::decode(frame, update, error))
                break;
            const auto peer = peers_.find(update.peer_id);
            if (peer != peers_.end()) {
                peer->second.peer.modules = std::move(update.modules);
                refresh_peer(peer->second, LANTERN_PEER_MANIFEST_CHANGED);
            }
            break;
        }
        case MessageType::ModuleData: {
            ModuleData message;
            if (!lantern::protocol::decode(frame, message, error))
                break;
            dispatch_message(message);
            break;
        }
        case MessageType::Ping: {
            uint64_t nonce = 0;
            if (lantern::protocol::decode_nonce(frame, nonce, error)) {
                std::vector<uint8_t> pong;
                if (lantern::protocol::encode_nonce(MessageType::Pong, nonce, pong, error)) {
                    network_.send(std::move(pong), false, "pong");
                }
            }
            break;
        }
        case MessageType::Pong:
        case MessageType::Hello:
            break;
        }
        if (!error.empty()) {
            svc_log->warn(mod_ctx, ("Lantern dropped malformed message: " + error).c_str());
        }
    }

    void add_peer(PeerManifest manifest_value, bool notify) {
        PeerState state;
        state.peer = std::move(manifest_value);
        const auto peer_id = state.peer.peer_id;
        peers_[peer_id] = std::move(state);
        refresh_peer(peers_.at(peer_id),
            notify ? LANTERN_PEER_JOINED : LANTERN_PEER_MANIFEST_CHANGED, notify);
    }

    void remove_peer(uint64_t peer_id) {
        const auto it = peers_.find(peer_id);
        if (it == peers_.end())
            return;
        LanternPeerInfo info = LANTERN_PEER_INFO_INIT;
        fill_peer_info(it->second, info);
        for_each_registration([&](Registration& registration) {
            if (registration.on_peer) {
                registration.on_peer(
                    registration.owner, LANTERN_PEER_LEFT, &info, registration.user_data);
            }
        });
        peers_.erase(it);
        recompute_compatibility();
    }

    void refresh_peer(PeerState& peer, LanternPeerEvent event, bool notify = true) {
        peer.compatibility = lantern::protocol::compare_manifests(manifest(), peer.peer.modules);
        peer.compatible_modules.clear();
        for (const auto& id : peer.compatibility.compatible_modules) {
            peer.compatible_modules.insert(id);
        }
        const std::string report = peer.compatibility.describe();
        LanternPeerInfo info = LANTERN_PEER_INFO_INIT;
        fill_peer_info(peer, info);
        for_each_registration([&](Registration& registration) {
            if (notify && registration.on_peer) {
                registration.on_peer(registration.owner, event, &info, registration.user_data);
            }
            if (registration.on_manifest) {
                registration.on_manifest(
                    registration.owner, peer.peer.peer_id, report.c_str(), registration.user_data);
            }
        });
        recompute_compatibility();
    }

    void dispatch_message(const ModuleData& message) {
        const auto peer = peers_.find(message.sender_peer_id);
        const auto registration_id = registration_by_id_.find(message.module_id);
        if (peer == peers_.end() || registration_id == registration_by_id_.end() ||
            !peer->second.compatible_modules.contains(message.module_id))
        {
            return;
        }
        auto& peer_state = peer->second;
        if (message.delivery == Delivery::UnreliableLatest) {
            auto [it, inserted] =
                peer_state.last_unreliable_sequence.emplace(message.module_id, message.sequence);
            if (!inserted) {
                if (!lantern::protocol::sequence_newer(message.sequence, it->second))
                    return;
                it->second = message.sequence;
            }
        }
        auto& registration = registrations_.at(registration_id->second);
        if (registration.on_message == nullptr)
            return;
        LanternMessage callback_message{
            .struct_size = sizeof(LanternMessage),
            .sender_peer_id = message.sender_peer_id,
            .delivery = message.delivery == Delivery::ReliableOrdered ? LANTERN_RELIABLE_ORDERED :
                                                                        LANTERN_UNRELIABLE_LATEST,
            .sequence = message.sequence,
            .data = message.payload.data(),
            .size = message.payload.size(),
        };
        registration.on_message(
            registration.owner, registration.handle, &callback_message, registration.user_data);
    }

    void clear_session(const std::string& detail, LanternSessionEvent event) {
        if (state_ == LANTERN_DISCONNECTED && peers_.empty() && self_peer_id_ == 0)
            return;
        for (const auto& [id, peer] : peers_) {
            (void)id;
            LanternPeerInfo info = LANTERN_PEER_INFO_INIT;
            fill_peer_info(peer, info);
            for_each_registration([&](Registration& registration) {
                if (registration.on_peer) {
                    registration.on_peer(
                        registration.owner, LANTERN_PEER_LEFT, &info, registration.user_data);
                }
            });
        }
        peers_.clear();
        self_peer_id_ = 0;
        state_ = LANTERN_DISCONNECTED;
        if (!detail.empty())
            compatibility_report_ = detail;
        notify_session(event, detail.c_str());
    }

    void notify_session(LanternSessionEvent event, const char* detail) {
        for_each_registration([&](Registration& registration) {
            if (registration.on_session) {
                registration.on_session(registration.owner, event, detail, registration.user_data);
            }
        });
    }

    template <typename Function>
    void for_each_registration(Function&& function) {
        // Copy handles: a callback may unregister itself without invalidating this traversal.
        std::vector<LanternModuleHandle> handles;
        handles.reserve(registrations_.size());
        for (const auto& [handle, registration] : registrations_) {
            (void)registration;
            handles.push_back(handle);
        }
        for (const auto handle : handles) {
            const auto it = registrations_.find(handle);
            if (it != registrations_.end())
                function(it->second);
        }
    }

    void recompute_compatibility() {
        std::string report =
            peers_.empty() ? "No peers connected" : "All connected peers compatible";
        for (const auto& [id, peer] : peers_) {
            if (!peer.compatibility.can_join || !peer.compatibility.issues.empty()) {
                report += "\n" + peer.peer.display_name + " (#" + std::to_string(id) +
                          "): " + peer.compatibility.describe();
            }
        }
        compatibility_report_ = std::move(report);
    }

    static void fill_peer_info(const PeerState& state, LanternPeerInfo& out) {
        out.peer_id = state.peer.peer_id;
        out.display_name = state.peer.display_name.c_str();
        out.color_rgb = state.peer.color_rgb;
        out.compatible_module_count = static_cast<uint32_t>(state.compatible_modules.size());
    }

    void register_settings() {
        server_config_ = register_config("server", CONFIG_VAR_STRING, 0, "127.0.0.1:43384");
        display_name_config_ = register_config("display-name", CONFIG_VAR_STRING, 0, "Player");
        color_config_ = register_config("color", CONFIG_VAR_INT, 0x77C8FF, nullptr);
        room_code_config_ = register_config("room-code", CONFIG_VAR_STRING, 0, "");
    }

    ConfigVarHandle register_config(
        const char* name, ConfigVarType type, int64_t default_int, const char* default_string) {
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = name;
        desc.type = type;
        desc.default_int = default_int;
        desc.default_string = default_string;
        ConfigVarHandle handle = 0;
        if (svc_config->register_var(mod_ctx, &desc, &handle) != MOD_OK)
            return 0;
        return handle;
    }

    std::string config_string(ConfigVarHandle handle, const char* fallback) const {
        if (handle == 0)
            return fallback;
        size_t length = 0;
        if (svc_config->get_string(mod_ctx, handle, nullptr, 0, &length) != MOD_OK)
            return fallback;
        std::string value(length + 1, '\0');
        if (svc_config->get_string(mod_ctx, handle, value.data(), value.size(), nullptr) != MOD_OK)
        {
            return fallback;
        }
        value.resize(length);
        return value;
    }

    int64_t config_int(ConfigVarHandle handle, int64_t fallback) const {
        int64_t value = fallback;
        return handle != 0 && svc_config->get_int(mod_ctx, handle, &value) == MOD_OK ? value :
                                                                                       fallback;
    }

    void register_ui() {
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = &LanternCore::build_panel;
        panel.update = &LanternCore::update_panel;
        panel.user_data = this;
        svc_ui->register_mods_panel(mod_ctx, &panel);
    }

    static ModResult build_panel(
        ModContext* ctx, UiElementHandle panel, void* user_data, ModError*) {
        auto& self = *static_cast<LanternCore*>(user_data);
        self.status_element_ = 0;
        self.peers_element_ = 0;
        self.compatibility_element_ = 0;
        svc_ui->pane_add_section(ctx, panel, "Room");
        self.add_bound_control(ctx, panel, UI_CONTROL_STRING, "Server address",
            "Host and UDP port for the Lantern relay.", self.server_config_, 128);
        self.add_bound_control(ctx, panel, UI_CONTROL_STRING, "Display name",
            "Name shown to connected peers.", self.display_name_config_,
            static_cast<int32_t>(lantern::protocol::kMaxNameBytes));
        self.add_bound_control(ctx, panel, UI_CONTROL_NUMBER, "Player color",
            "RGB color encoded as a decimal number.", self.color_config_, 0);
        self.add_bound_control(ctx, panel, UI_CONTROL_STRING, "Invite code",
            "Create a room to receive a code, or paste one here before joining.",
            self.room_code_config_, static_cast<int32_t>(lantern::protocol::kMaxRoomCodeBytes));
        self.add_button(ctx, panel, "Create room", &LanternCore::create_pressed);
        self.add_button(ctx, panel, "Join room", &LanternCore::join_pressed);
        self.add_button(ctx, panel, "Disconnect", &LanternCore::disconnect_pressed);
        svc_ui->pane_add_section(ctx, panel, "Session");
        svc_ui->pane_add_text(ctx, panel, "", &self.status_element_);
        svc_ui->pane_add_text(ctx, panel, "", &self.peers_element_);
        svc_ui->pane_add_section(ctx, panel, "Compatibility");
        svc_ui->pane_add_text(ctx, panel, "", &self.compatibility_element_);
        self.refresh_ui(ctx);
        return MOD_OK;
    }

    static ModResult update_panel(ModContext* ctx, void* user_data, ModError*) {
        static_cast<LanternCore*>(user_data)->refresh_ui(ctx);
        return MOD_OK;
    }

    void add_bound_control(ModContext* ctx, UiElementHandle panel, UiControlKind kind,
        const char* label, const char* help, ConfigVarHandle config, int32_t max_length) {
        UiControlDesc desc = UI_CONTROL_DESC_INIT;
        desc.kind = kind;
        desc.label = label;
        desc.help_rml = help;
        desc.binding = UI_BINDING_CONFIG_VAR;
        desc.config_var = config;
        desc.max_length = max_length;
        if (kind == UI_CONTROL_NUMBER) {
            desc.min = 0;
            desc.max = 0xFFFFFF;
            desc.step = 1;
        }
        svc_ui->pane_add_control(ctx, panel, &desc, nullptr);
    }

    void add_button(
        ModContext* ctx, UiElementHandle panel, const char* label, UiPressedFn callback) {
        UiControlDesc desc = UI_CONTROL_DESC_INIT;
        desc.kind = UI_CONTROL_BUTTON;
        desc.label = label;
        desc.on_pressed = callback;
        desc.user_data = this;
        svc_ui->pane_add_control(ctx, panel, &desc, nullptr);
    }

    static void create_pressed(ModContext*, void* user_data) {
        static_cast<LanternCore*>(user_data)->create_room();
    }
    static void join_pressed(ModContext*, void* user_data) {
        static_cast<LanternCore*>(user_data)->join_room();
    }
    static void disconnect_pressed(ModContext*, void* user_data) {
        static_cast<LanternCore*>(user_data)->disconnect();
    }

    void refresh_ui(ModContext* ctx) {
        static constexpr const char* states[] = {
            "Disconnected", "Connecting", "Negotiating manifests", "Connected", "Reconnecting"};
        std::string status = std::string("Status: ") + states[static_cast<size_t>(state_)];
        if (!room_code_.empty())
            status += " | Room: " + room_code_;
        std::string peer_text = "Peers: " + std::to_string(peers_.size());
        for (const auto& [id, peer] : peers_) {
            peer_text += "\n- " + peer.peer.display_name + " (#" + std::to_string(id) + ")";
        }
        if (status_element_)
            svc_ui->elem_set_text(ctx, status_element_, status.c_str());
        if (peers_element_)
            svc_ui->elem_set_text(ctx, peers_element_, peer_text.c_str());
        if (compatibility_element_) {
            svc_ui->elem_set_text(ctx, compatibility_element_, compatibility_report_.c_str());
        }
    }

    lantern::net::NetworkClient network_;
    LanternConnectionState state_ = LANTERN_DISCONNECTED;
    LanternPeerId self_peer_id_ = 0;
    LanternModuleHandle next_registration_ = 1;
    std::map<LanternModuleHandle, Registration> registrations_;
    std::unordered_map<std::string, LanternModuleHandle> registration_by_id_;
    std::map<LanternPeerId, PeerState> peers_;
    std::string room_code_;
    std::string compatibility_report_ = "Not connected";
    uint64_t lifecycle_watch_ = 0;

    ConfigVarHandle server_config_ = 0;
    ConfigVarHandle display_name_config_ = 0;
    ConfigVarHandle color_config_ = 0;
    ConfigVarHandle room_code_config_ = 0;
    UiElementHandle status_element_ = 0;
    UiElementHandle peers_element_ = 0;
    UiElementHandle compatibility_element_ = 0;
};

LanternCore g_core;

ModResult service_register_module(
    ModContext* owner, const LanternModuleDesc* desc, LanternModuleHandle* out_handle) {
    return g_core.register_module(owner, desc, out_handle);
}
ModResult service_unregister_module(ModContext* owner, LanternModuleHandle handle) {
    return g_core.unregister_module(owner, handle);
}
ModResult service_send(ModContext* owner, const LanternSendDesc* desc) {
    return g_core.send(owner, desc);
}
LanternConnectionState service_connection_state(ModContext*) {
    return g_core.connection_state();
}
LanternPeerId service_self_peer_id(ModContext*) {
    return g_core.self_peer_id();
}
size_t service_peer_count(ModContext*) {
    return g_core.peer_count();
}
ModResult service_peer_at(ModContext*, size_t index, LanternPeerInfo* out) {
    return g_core.peer_at(index, out);
}
const char* service_room_code(ModContext*) {
    return g_core.room_code();
}
const char* service_compatibility_report(ModContext*) {
    return g_core.compatibility_report();
}

constexpr LanternService lantern_service{
    .header = SERVICE_HEADER(LanternService, LANTERN_SERVICE_MAJOR, LANTERN_SERVICE_MINOR),
    .register_module = service_register_module,
    .unregister_module = service_unregister_module,
    .send = service_send,
    .connection_state = service_connection_state,
    .self_peer_id = service_self_peer_id,
    .peer_count = service_peer_count,
    .peer_at = service_peer_at,
    .room_code = service_room_code,
    .compatibility_report = service_compatibility_report,
};
EXPORT_SERVICE(lantern_service);

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    return g_core.initialize(error);
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    return g_core.update(error);
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    return g_core.shutdown(error);
}
}
