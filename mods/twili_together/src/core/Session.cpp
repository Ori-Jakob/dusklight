#include "core/Session.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "enemy/EnemyScaling.hpp"
#include "enemy/EnemySync.hpp"
#include "story/Story.hpp"
#include "sync/WorldSync.hpp"
#include "teleport/Teleport.hpp"

#if TWILI_ENABLE_AUTOTEST
#include "autotest/AutoTest.hpp"
#endif

#include <mods/svc/host.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

namespace twili {
namespace {

using Clock = std::chrono::steady_clock;

// Time spent on received packets per tick; the rest waits for the next one.
constexpr auto kPacketBudget = std::chrono::milliseconds(4);

Session* s_session = nullptr;

std::string trimAsciiWhitespace(std::string value) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

}  // namespace

void Session::init() {
    if (s_session == nullptr) {
        s_session = new Session();
    }
}

void Session::shutdown() {
    delete s_session;
    s_session = nullptr;
}

bool Session::active() {
    return s_session != nullptr;
}

Session& Session::instance() {
    return *s_session;
}

Session::Session() {
    // Per process: a restarted game may hold an older save than its last session sent.
    std::random_device rd;
    mSessionKey = fmt::format("{:08x}{:08x}", rd(), rd());
}

Session::~Session() {
    mLink.stop();
}

void Session::resetSessionState() {
    mClients.clear();
    mSelfClientId = 0;
    std::memset(mLastStageName, 0, sizeof(mLastStageName));
    mLastLayerNo = -127;
    mLastRoomNo = -127;
    mLastSaveTblNo = -2;
    mLastSaveLoaded = false;
    mRoomState = RoomState{};
    mVersionMismatchLogged.clear();
    sync::resetSession();
    enemy_sync::resetSession();
    story::resetSession();
}

void Session::connect() {
    mParamsOverride = false;
    mParams = {};
    startConnect();
}

void Session::connect(const ConnectParams& params) {
    mParamsOverride = true;
    mParams = params;
    startConnect();
}

void Session::startConnect() {
    if (!mParamsOverride) {
        mParams.url = config::getString(config::Var::ServerUrl);
        mParams.name = config::getString(config::Var::DisplayName);
        mParams.room = config::getString(config::Var::RoomId);
        mParams.team = config::getString(config::Var::TeamId);
    }
    const std::string url = trimAsciiWhitespace(mParams.url);
    mConnectionFailureMessage.clear();
    mLastCloseReason.clear();
    resetSessionState();
    destroyDummies();
    if (url.empty()) {
        TwiliLog.warn("[net] no server URL configured");
        mConnectionFailureMessage = "No server URL set.";
        return;
    }
    TwiliLog.info("[net] connecting to {}", url);
    mLastSaveLoaded = isSaveLoaded();
    std::string error;
    if (!mLink.start(url, config::getBool(config::Var::AutoReconnect), error)) {
        TwiliLog.warn("[net] cannot connect to {}: {}", url, error);
        mConnectionFailureMessage = "Could not connect: " + error;
    }
}

void Session::disconnect() {
    mLink.stop();
    mConnectionFailureMessage.clear();
    resetSessionState();
    destroyDummies();
}

SessionState Session::state() const {
    switch (mLink.state()) {
    case net::LinkState::Open:
        return SessionState::Connected;
    case net::LinkState::Connecting:
    case net::LinkState::Backoff:
        return SessionState::Connecting;
    default:
        return SessionState::Disconnected;
    }
}

std::string Session::statusText() const {
    switch (mLink.state()) {
    case net::LinkState::Idle:
        return mConnectionFailureMessage.empty() ? "Disconnected" : mConnectionFailureMessage;
    case net::LinkState::Connecting:
        return mLink.attempt() > 0 ? fmt::format("Reconnecting to {}...", mLink.url()) :
                                     fmt::format("Connecting to {}...", mLink.url());
    case net::LinkState::Backoff:
        return fmt::format("Connection lost ({}), retrying in {}s", mLastCloseReason,
            mLink.retryInSeconds());
    case net::LinkState::Open:
        if (mSelfClientId == 0) {
            return "Joining...";
        }
        return fmt::format("Connected via {} as client {} ({} other player(s))",
            mLink.transportName(), mSelfClientId, mClients.empty() ? 0 : mClients.size() - 1);
    }
    return {};
}

const Client* Session::selfClient() const {
    const auto it = mClients.find(mSelfClientId);
    return it == mClients.end() ? nullptr : &it->second;
}

const std::string& Session::selfTeamId() const {
    if (const Client* self = selfClient()) {
        return self->teamId;
    }
    return mParams.team;
}

bool Session::isTeammate(const Client& client) const {
    return client.teamId == selfTeamId();
}

bool Session::send(const nlohmann::json& packet, net::Delivery delivery) {
    return mLink.send(packet, delivery);
}

void Session::update() {
    mLink.setAutoReconnect(config::getBool(config::Var::AutoReconnect));
    mLink.pump();
    const auto deadline = Clock::now() + kPacketBudget;
    net::LinkEvent ev;
    while (Clock::now() < deadline && mLink.next(ev)) {
        switch (ev.type) {
        case net::LinkEvent::Type::Opened:
            onConnected();
            break;
        case net::LinkEvent::Type::Closed:
            onDisconnected(ev.reason, ev.retrying);
            break;
        case net::LinkEvent::Type::Message:
            // json::value() throws on a type mismatch: a malformed packet costs that packet.
            try {
                onMessage(ev.packet);
            } catch (const std::exception& e) {
                TwiliLog.warn("[net] dropped malformed packet: {}", e.what());
            }
            break;
        }
    }
    // Before stage tracking: a kill made right before a stage change goes where the enemy lived.
    enemy_sync::flush();
    if (isConnected()) {
        tickStageTracking();
        tickSelfColor();
        sync::tick();
        tickRoomOwnerSettings();
        manageDummyActors();
    }
    // Connected or not: a stage change already under way is seen through.
    enemy_sync::tick();
    teleport::tick();
    story::tick();
    enemy_scaling::tick();
}

void Session::onConnected() {
    TwiliLog.info("[net] connected to {} over {}", mLink.url(), mLink.transportName());
    mConnectionFailureMessage.clear();
    resetSessionState();
    mLastSaveLoaded = isSaveLoaded();
    sendHandshake();
}

void Session::onDisconnected(const std::string& reason, bool retrying) {
    const bool wasJoined = mSelfClientId != 0;
    mLastCloseReason = reason.empty() ? "closed" : reason;
    if (retrying) {
        if (wasJoined) {
            TwiliLog.warn("[net] connection lost ({}), reconnecting", mLastCloseReason);
        }
    } else {
        TwiliLog.info("[net] disconnected ({})", mLastCloseReason);
        if (mConnectionFailureMessage.empty()) {
            mConnectionFailureMessage = wasJoined ? "Connection lost: " + mLastCloseReason :
                                                    "Could not connect: " + mLastCloseReason;
        }
    }
    resetSessionState();
    destroyDummies();
}

// Dummies still being created delete themselves once their client is gone.
void Session::destroyDummies() {
    for (const auto& [id, pid] : mDummyActors) {
        if (fopAcM_IsExecuting(pid)) {
            fopAcM_delete(pid);
        }
    }
    mDummyActors.clear();
    mDummySeenExecuting.clear();
    mDummyCreateFailures.clear();
}

void Session::onMessage(const nlohmann::json& packet) {
    const auto type = packet.value("type", std::string{});
    // The server stamps clientId on everything it relays, so a stamped copy is spoofed.
    if ((type == "SERVER_MESSAGE" || type == "DISABLE_CLIENT") && packet.contains("clientId")) {
        TwiliLog.warn("[net] ignoring {} relayed from client {}", type, packet["clientId"].dump());
        return;
    }
    if (type == "ALL_CLIENT_STATE") {
        handleAllClientState(packet);
    } else if (type == "UPDATE_CLIENT_STATE") {
        handleUpdateClientState(packet);
    } else if (type == "PLAYER_UPDATE") {
        handlePlayerUpdate(packet);
    } else if (type == "PLAYER_SFX") {
        handlePlayerSfx(packet);
    } else if (type == "UPDATE_ROOM_STATE") {
        handleUpdateRoomState(packet);
    } else if (sync::handlePacket(type, packet)) {
    } else if (teleport::handlePacket(type, packet)) {
    } else if (enemy_sync::handlePacket(type, packet)) {
    } else if (story::handlePacket(type, packet)) {
    } else if (type == "AUTOTEST_SIGNAL") {
#if TWILI_ENABLE_AUTOTEST
        autotest::onSignal(packet.value("instance", std::string{}),
            packet.value("name", std::string{}));
#endif
    } else if (type == "SERVER_MESSAGE") {
        TwiliLog.info("[net] server message: {}", packet.value("message", std::string{}));
    } else if (type == "DISABLE_CLIENT") {
        const std::string message = packet.value("message", std::string{});
        TwiliLog.warn("[net] server requested disconnect: {}", message);
        disconnect();
        mConnectionFailureMessage =
            message.empty() ? "Disconnected by server." : "Disconnected by server: " + message;
    }
}

void Session::sendHandshake() {
    const auto color = localPlayerColor();
    rememberSentColor(color);
    send({
        {"type", "HANDSHAKE"},
        {"app", net::kApp},
        {"protocolVersion", kProtocolVersion},
        {"modVersion", std::string(svc_host->mod_version(mod_ctx))},
        {"layout", sync::localLayout()},
        {"transport", std::string(mLink.transportName())},
        {"name", mParams.name},
        {"teamId", mParams.team},
        {"roomId", trimAsciiWhitespace(mParams.room)},
        {"sessionKey", mSessionKey},
        {"color", {{"r", color[0]}, {"g", color[1]}, {"b", color[2]}}},
    });
}

void Session::sendAutotestSignal(const std::string& instance, const std::string& name) {
    send({{"type", "AUTOTEST_SIGNAL"}, {"instance", instance}, {"name", name}});
}

void Session::onSelfStateKnown() {
    if (mSelfClientId != 0) {
        mLink.markJoined();
        sync::requestExchange();
    }
}

void Session::queuePvpHit(uint32_t, const pvp::HitReport&) {}

bool Session::storySharedEventWith(uint32_t clientId) const {
    return story::sharedEventWith(clientId);
}

}  // namespace twili
