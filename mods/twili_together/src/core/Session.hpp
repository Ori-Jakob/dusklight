#pragma once

#include "core/Client.hpp"
#include "core/RoomState.hpp"
#include "net/Link.hpp"
#include "net/Protocol.hpp"
#include "pvp/Pvp.hpp"

#include "f_op/f_op_actor_mng.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>

class daAlink_c;

namespace twili {

enum class SessionState { Disconnected, Connecting, Connected };

// The color setting, "rrggbb" with an optional '#'; white when unreadable.
std::array<int, 3> localPlayerColor();

enum class PlayerSfxKind : uint8_t {
    Voice = 0,
    VoiceLevel = 1,
    Sword = 2,
    Sound = 3,
    SoundLevel = 4,
    MapInfo = 5,
    MapInfoLevel = 6,
};

struct ConnectParams {
    std::string url;
    std::string name;
    std::string room;
    std::string team;
};

// The multiplayer hub: connection, room roster, presence and the dummies.
class Session {
public:
    // Clients on another protocol see each other in the room but exchange no presence.
    static constexpr int kProtocolVersion = net::kProtocolVersion;

    static void init();
    static void shutdown();
    static bool active();
    static Session& instance();

    // Connects with the settings, or with explicit values (autotest).
    void connect();
    void connect(const ConnectParams& params);
    void disconnect();

    SessionState state() const;
    bool isConnected() const { return state() == SessionState::Connected; }
    bool joined() const { return isConnected() && mSelfClientId != 0; }
    // Set when a connection attempt failed for good (no reconnect follows).
    const std::string& connectionFailureMessage() const { return mConnectionFailureMessage; }
    std::string statusText() const;

    const std::map<uint32_t, Client>& clients() const { return mClients; }
    fopAc_ac_c* dummyActorForClient(uint32_t clientId) const;

    uint32_t selfClientId() const { return mSelfClientId; }
    const Client* selfClient() const;
    const std::string& selfTeamId() const;
    uint32_t colorPushCount() const { return mColorPushCount; }

    const RoomState& roomState() const { return mRoomState; }
    bool isRoomOwner() const {
        return isConnected() && mSelfClientId != 0 && mRoomState.ownerClientId == mSelfClientId;
    }
    bool isTeammate(const Client& client) const;
    const std::string& sessionKey() const { return mSessionKey; }
    // Gate for world sync with a member (ourselves included); the randomizer game key goes here.
    bool memberMaySync(uint32_t clientId) const {
        (void)clientId;
        return true;
    }

    // Once per simulation tick.
    void update();

    void sendPlayerUpdate(float posX, float posY, float posZ, int16_t angleX, int16_t angleY,
        int16_t angleZ, int16_t shapeAngleX, int16_t shapeAngleY, int16_t shapeAngleZ,
        int16_t bodyAngleX, int16_t bodyAngleY, int16_t bodyAngleZ, int16_t bodyTwistY,
        bool attentionLock, bool shieldInHand, int8_t transformStatus, bool modelSwap,
        const uint16_t upperANMs[3], const uint16_t lowerANMs[3], const float upperFrames[3],
        const float lowerFrames[3], const float upperRatios[3], const float lowerRatios[3],
        const int16_t waterDropColors[2][4], const int16_t swordUpColors[2][4],
        uint8_t swordItem, uint8_t shieldItem, uint8_t clothesItem, uint16_t equipItem,
        uint8_t upperBlendMode, float upperBlendRatio, uint8_t cutType, bool swordBlurActive,
        uint8_t swordBlurAlpha, uint8_t leftHandIndex, uint8_t rightHandIndex,
        uint8_t leftHandItemOverride, uint8_t rightHandItemOverride,
        uint8_t leftHandGripOverride, uint8_t rightHandGripOverride, uint16_t leftItemJoint,
        uint16_t rightItemJoint, uint16_t itemBckId, float itemBckFrame, bool itemAmmoLoaded,
        uint16_t itemProjectileSeq, uint8_t itemProjectileType, bool swordChargeActive,
        float swordChargeFrame, uint16_t visFlags);
    // The PLAYER_UPDATE clip id of body pack `pack`, 0 for none or the lower clip of the pack.
    static uint16_t playerPackAnmId(daAlink_c& link, bool upper, int pack);
    // Midna on the local wolf's back; false while peers are to show none.
    static bool captureLocalMidna(bool inCutscene, RemoteMidnaPose& out);
    static void captureLocalTransformFx(RemoteTransformFx& out);
    // Clip `resIdx` of player animation archive `arcNo` (1-8) if resident, else null.
    static void* residentPlayerArcAnm(uint16_t arcNo, uint16_t resIdx);
    // Autotest: merges `patch` into the next `packets` PLAYER_UPDATEs, each a keyframe.
    static void setPlayerUpdateTestPatch(const nlohmann::json& patch, int packets);
    // Our PLAYER_UPDATE seq.
    static uint32_t localPoseSeq();
    void sendPlayerSfx(uint32_t soundId, PlayerSfxKind kind, uint32_t mapInfo = 0);

    // dSv_info_c::getSave / dStage_Delete.
    void onStageSaveTableLoaded(int saveTblNo);
    void onStageSaveTableUnloaded();
    int currentSaveTblNo() const { return mCurrentSaveTblNo; }

    // Owner only; the server rejects everyone else.
    void sendUpdateRoomState();

    // Autotest barrier; the server relays it to the room.
    void sendAutotestSignal(const std::string& instance, const std::string& name);

    // PvP (P5): a hit our collision pass registered on a dummy.
    void queuePvpHit(uint32_t victimId, const pvp::HitReport& hit);
    // Story sync (P5): we share a cutscene with this client, so its dummy hides.
    bool storySharedEventWith(uint32_t clientId) const;

    // Presence packets may be dropped under pressure; false when this one was.
    bool send(const nlohmann::json& packet, net::Delivery delivery = net::Delivery::Reliable);

private:
    Session();
    ~Session();

    void startConnect();
    void onConnected();
    void onDisconnected(const std::string& reason, bool retrying);
    void onMessage(const nlohmann::json& packet);
    void resetSessionState();

    void sendHandshake();
    void tickStageTracking();
    void tickSelfColor();
    void rememberSentColor(const std::array<int, 3>& color);
    // The server relays our client state to everyone but us: our own row takes what we sent.
    void syncSelfRow();
    void tickRoomOwnerSettings();
    void onSelfStateKnown();

    void handleAllClientState(const nlohmann::json& packet);
    void handleUpdateClientState(const nlohmann::json& packet);
    void handlePlayerUpdate(const nlohmann::json& packet);
    void handlePlayerSfx(const nlohmann::json& packet);
    void handleUpdateRoomState(const nlohmann::json& packet);

    RoomState roomStateFromSettings() const;

    void manageDummyActors();
    void destroyDummies();
    bool clientIsInCurrentLayer(const Client& client) const;
    bool clientHasPlayerInCurrentLayer(const Client& client) const;
    bool hasRemoteClientInCurrentLayer() const;

    net::Link mLink;
    ConnectParams mParams;
    bool mParamsOverride = false;
    std::map<uint32_t, Client> mClients;
    std::map<uint32_t, fpc_ProcID> mDummyActors;
    // Dummies that reached execute; one that vanishes before that failed create().
    std::set<uint32_t> mDummySeenExecuting;
    struct DummyCreateFailure {
        int count = 0;
        std::chrono::steady_clock::time_point retryAt{};
    };
    std::map<uint32_t, DummyCreateFailure> mDummyCreateFailures;
    std::chrono::steady_clock::time_point mSceneChangedAt{};
    std::string mConnectionFailureMessage;
    std::string mLastCloseReason;
    uint32_t mSelfClientId = 0;
    char mLastStageName[8] = {};
    int8_t mLastLayerNo = -127;
    int8_t mLastRoomNo = -127;
    bool mLastSaveLoaded = false;
    int mCurrentSaveTblNo = -1;
    int mLastSaveTblNo = -2;
    std::array<int, 3> mSentColor{-1, -1, -1};
    std::chrono::steady_clock::time_point mSentColorAt{};
    uint32_t mColorPushCount = 0;

    RoomState mRoomState;
    std::chrono::steady_clock::time_point mLastRoomStatePush{};
    RoomState mLastPushedRoomState{};

    std::set<uint32_t> mVersionMismatchLogged;

    // Random per process: packets we sent under an earlier clientId are never applied to us.
    std::string mSessionKey;
};

}  // namespace twili
