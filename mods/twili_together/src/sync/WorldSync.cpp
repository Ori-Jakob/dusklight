// Team-scoped world packets: the gate, queue numbering, the save exchange and the layout check.

#include "sync/WorldSyncState.hpp"

#include "core/Layout.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "sync/RemoteApplyGuard.hpp"
#include "ui/Toasts.hpp"

#include <fmt/format.h>

namespace twili::sync {
namespace detail {
namespace {

// Answers to our REQUEST_WORLD_STATE get this long to merge before our own state is published.
constexpr auto kOwnStatePublishDelay = std::chrono::milliseconds(1500);
// Twice the server's queue length: anything older is never replayed.
constexpr size_t kMaxQueueApplied = 8000;

State s_state;

void warnLayoutMismatch(uint32_t clientId, const std::string& name, const std::string& layout) {
    State& st = s_state;
    if (!st.layoutWarnedIds.insert(clientId).second) {
        return;
    }
    const std::string shown = name.empty() ? fmt::format("Player {}", clientId) : name;
    st.layoutWarnedNames.insert(shown);
    st.stats.layoutWarnings++;
    TwiliLog.warn(
        "[sync] {} (client {}) uses save layout {}, we use {}: world sync with them is off", shown,
        clientId, layout.empty() ? "?" : layout, localLayout());
    ui::toastRml("Twili-Together",
        fmt::format("<b>{}</b> runs a different game version. World sync with them is off.",
            ui::escapeRml(shown)),
        ui::kToastWarning, 8000);
}

void checkRosterLayouts() {
    const Session& session = Session::instance();
    for (const auto& [id, client] : session.clients()) {
        if (client.self || !client.online || client.layout.empty() ||
            client.teamId != session.selfTeamId() || client.layout == localLayout())
        {
            continue;
        }
        warnLayoutMismatch(id, client.name, client.layout);
    }
}

}  // namespace

State& state() {
    return s_state;
}

int currentSaveTblNo() {
    return Session::instance().currentSaveTblNo();
}

void send(nlohmann::json packet) {
    Session::instance().send(packet);
}

void checkSaveGeneration() {
    State& st = s_state;
    const uint32_t generation = saveGeneration();
    if (generation == st.saveGeneration) {
        return;
    }
    const bool hadSave = st.saveGeneration != 0;
    st.saveGeneration = generation;
    if (!hadSave) {
        return;
    }
    // Nothing of the old save applies to this one: catch up again from scratch.
    TwiliLog.info("[sync] another save was loaded; the next exchange catches up again");
    st.queueApplied.clear();
    st.cleared = {};
    st.keyCountAdoptable = 0;
    st.catchUpRequested = false;
    st.dungeonBaselineValid = false;
    st.pendingGrants.clear();
    st.exchangePending = true;
}

bool acceptsWorldPacket(const nlohmann::json& packet, const char* what) {
    State& st = s_state;
    if (!enabled() || !isSaveLoaded()) {
        return false;
    }
    const Session& session = Session::instance();
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == session.selfClientId()) {
        return false;
    }
    // Our own packets from before a reconnect carry an old clientId.
    if (packet.value("senderSessionKey", std::string{}) == session.sessionKey()) {
        return false;
    }
    if (packet.value("teamId", std::string{}) != session.selfTeamId()) {
        return false;
    }
    const int protocolVersion = packet.value("protocolVersion", -1);
    if (protocolVersion != Session::kProtocolVersion) {
        if (st.versionMismatchLogged.insert(id).second) {
            TwiliLog.warn("[sync] dropping {} from client {} (protocol {} != {})", what, id,
                protocolVersion, Session::kProtocolVersion);
        }
        return false;
    }
    const std::string layout = packet.value("layout", std::string{});
    if (layout != localLayout()) {
        st.stats.layoutRefusals++;
        warnLayoutMismatch(id, packet.value("senderName", std::string{}), layout);
        return false;
    }
    if (!session.memberMaySync(id)) {
        return false;
    }

    // A reconnect's catch-up replays the whole queue; what already applied must not apply twice.
    const auto seq = packet.find("queueSeq");
    const auto epoch = packet.find("queueEpoch");
    if (seq != packet.end() && seq->is_number_unsigned() && epoch != packet.end() &&
        epoch->is_string())
    {
        checkSaveGeneration();
        if (epoch->get_ref<const std::string&>() != st.queueEpoch) {
            st.queueEpoch = epoch->get<std::string>();
            st.queueApplied.clear();
        }
        if (!st.queueApplied.insert(seq->get<uint64_t>()).second) {
            return false;
        }
        while (st.queueApplied.size() > kMaxQueueApplied) {
            st.queueApplied.erase(st.queueApplied.begin());
        }
    }
    return true;
}

void stampWorldPacket(nlohmann::json& packet, bool addToQueue) {
    packet["protocolVersion"] = Session::kProtocolVersion;
    packet["teamId"] = Session::instance().selfTeamId();
    packet["layout"] = localLayout();
    packet["addToQueue"] = addToQueue;
}

}  // namespace detail

using namespace detail;

bool enabled() {
    if (!Session::active()) {
        return false;
    }
    const Session& session = Session::instance();
    return session.isConnected() && session.roomState().syncWorldState &&
           session.memberMaySync(session.selfClientId());
}

const std::string& localLayout() {
    static const std::string kLayout = layout::saveLayoutHex();
    return s_state.layoutOverride.empty() ? kLayout : s_state.layoutOverride;
}

void setLayoutOverrideForTest(const std::string& layout) {
    s_state.layoutOverride = layout;
    TwiliLog.info("[sync] save layout forced to {}", localLayout());
}

bool layoutCompatible(const Client& client) {
    return client.layout.empty() || client.layout == localLayout();
}

void stampPacket(nlohmann::json& packet, bool addToQueue) {
    stampWorldPacket(packet, addToQueue);
}

void requestExchange() {
    if (Session::active() && Session::instance().isConnected()) {
        s_state.exchangePending = true;
    }
}

void onStageSaveTableLoaded() {
    s_state.dungeonBaselineValid = false;
    if (Session::instance().currentSaveTblNo() >= 0) {
        requestExchange();
    }
}

void onStageSaveTableUnloaded() {
    s_state.dungeonBaselineValid = false;
}

bool handlePacket(const std::string& type, const nlohmann::json& packet) {
    if (type == "SET_FLAG") {
        handleSetFlag(packet);
    } else if (type == "UNSET_FLAG") {
        handleUnsetFlag(packet);
    } else if (type == "SET_EVENT_BIT") {
        handleSetEventBit(packet);
    } else if (type == "UNSET_EVENT_BIT") {
        handleUnsetEventBit(packet);
    } else if (type == "GIVE_ITEM") {
        handleGiveItem(packet);
    } else if (type == "UPDATE_DUNGEON_ITEMS") {
        handleUpdateDungeonItems(packet);
    } else if (type == "REQUEST_WORLD_STATE") {
        handleRequestWorldState(packet);
    } else if (type == "UPDATE_WORLD_STATE") {
        handleUpdateWorldState(packet);
    } else {
        return false;
    }
    return true;
}

void tick() {
    State& st = s_state;
    checkSaveGeneration();
    checkRosterLayouts();

    // Request first, publish later: publishing a stale save would replace the team's cached state.
    const Session& session = Session::instance();
    const bool canExchange =
        session.selfClientId() != 0 && isSaveLoaded() && currentSaveTblNo() >= 0 && enabled();
    const auto now = Clock::now();
    if (st.exchangePending && canExchange) {
        st.exchangePending = false;
        sendRequestWorldState();
        st.ownStatePublishAt = now + kOwnStatePublishDelay;
    }
    if (st.ownStatePublishAt != Clock::time_point{} && now >= st.ownStatePublishAt) {
        st.ownStatePublishAt = {};
        if (canExchange) {
            sendUpdateWorldState();
        }
    }
    tickDungeonItemTracking();
}

void endOfUpdate() {
    checkSaveGeneration();
    s_state.grantWindow = !s_state.pendingGrants.empty();
}

void resetSession() {
    State& st = s_state;
    st.exchangePending = false;
    st.catchUpRequested = false;
    st.ownStatePublishAt = {};
    st.cleared = {};
    st.keyCountAdoptable = 0;
    st.dungeonBaselineValid = false;
    st.versionMismatchLogged.clear();
    st.layoutWarnedIds.clear();
    st.merges.clear();
}

void shutdown() {
    removeItemObserver();
    s_state = State{};
    RemoteApplyGuard::reset();
}

void onSaveWritten() {
    if (Session::active() && Session::instance().isConnected()) {
        sendUpdateWorldState();
    }
}

bool ownStateSettled() {
    return !enabled() ||
           (!s_state.exchangePending && s_state.ownStatePublishAt == Clock::time_point{});
}

uint32_t mergeCount(uint32_t clientId) {
    const auto it = s_state.merges.find(clientId);
    return it == s_state.merges.end() ? 0 : it->second;
}

const Stats& stats() {
    return s_state.stats;
}

bool layoutWarned(const std::string& name) {
    return s_state.layoutWarnedNames.count(name) != 0;
}

}  // namespace twili::sync
