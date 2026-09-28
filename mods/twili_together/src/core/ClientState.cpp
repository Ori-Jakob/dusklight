#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"
#include "story/Story.hpp"
#include "sync/WorldSync.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "d/d_stage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace twili {

// At most 4 colour updates a second while the picker is dragged.
static constexpr auto kColorPushInterval = std::chrono::milliseconds(250);
// The server keeps at most 32 code points of a horse name.
static constexpr size_t kMaxHorseNameBytes = 128;

static const nlohmann::json& clientStatePayload(const nlohmann::json& packet) {
    const auto state = packet.find("state");
    if (state != packet.end() && state->is_object()) {
        return *state;
    }
    return packet;
}

static void copyStageName(Client& client, const std::string& stage) {
    const size_t len = (std::min)(stage.size(), size_t(7));
    std::memset(client.stageName, 0, sizeof(client.stageName));
    std::memcpy(client.stageName, stage.c_str(), len);
}

static void applyClientState(Client& client, const nlohmann::json& state) {
    if (state.contains("clientId")) client.clientId = state.value("clientId", client.clientId);
    if (state.contains("name")) client.name = state.value("name", std::string{});
    if (state.contains("teamId")) client.teamId = state.value("teamId", std::string{});
    if (state.contains("modVersion")) client.modVersion = state.value("modVersion", std::string{});
    if (state.contains("layout")) client.layout = state.value("layout", std::string{});
    if (state.contains("protocolVersion")) {
        client.protocolVersion = state.value("protocolVersion", 0);
    }
    if (state.contains("online")) client.online = state.value("online", false);
    if (state.contains("self")) client.self = state.value("self", false);
    if (state.contains("isSaveLoaded")) client.isSaveLoaded = state.value("isSaveLoaded", false);
    if (state.contains("stageName")) copyStageName(client, state.value("stageName", std::string{}));
    if (state.contains("layerNo")) client.layerNo = static_cast<int8_t>(state.value("layerNo", 0));
    if (state.contains("roomNo")) client.roomNo = static_cast<int8_t>(state.value("roomNo", 0));
    if (state.contains("saveTblNo")) {
        client.saveTblNo = static_cast<int8_t>(state.value("saveTblNo", -1));
    }

    if (const auto color = state.find("color"); color != state.end() && color->is_object()) {
        client.colorR = static_cast<uint8_t>(color->value("r", 255u));
        client.colorG = static_cast<uint8_t>(color->value("g", 255u));
        client.colorB = static_cast<uint8_t>(color->value("b", 255u));
    }
    if (const auto name = state.find("horseName"); name != state.end() && name->is_string()) {
        const std::string& value = name->get_ref<const std::string&>();
        client.horseName = value.size() <= kMaxHorseNameBytes ? value : std::string{};
    }
    // null while the owner is in its horse's stage (its stream shows her) or has none.
    if (const auto place = state.find("horsePlace"); place != state.end()) {
        client.horsePlace = HorsePlace{};
        const auto num = [&](const char* key, double& out) {
            const auto it = place->find(key);
            if (it == place->end() || !it->is_number()) return false;
            out = it->get<double>();
            return std::isfinite(out) && std::fabs(out) < 1.0e6;
        };
        double x = 0, y = 0, z = 0, room = 0, angle = 0;
        if (place->is_object()) {
            const auto stageIt = place->find("stage");
            if (stageIt != place->end() && stageIt->is_string() && num("x", x) && num("y", y) &&
                num("z", z) && num("room", room) && num("angleY", angle))
            {
                const std::string& stage = stageIt->get_ref<const std::string&>();
                if (!stage.empty() && stage.size() <= 7) {
                    HorsePlace& p = client.horsePlace;
                    p.valid = true;
                    std::memcpy(p.stage, stage.c_str(), stage.size());
                    p.room = static_cast<int8_t>(std::clamp(room, -128.0, 127.0));
                    p.pos[0] = static_cast<float>(x);
                    p.pos[1] = static_cast<float>(y);
                    p.pos[2] = static_cast<float>(z);
                    p.angleY = static_cast<int16_t>(std::clamp(angle, -32768.0, 32767.0));
                }
            }
        }
    }
}

// Positions describe the scene they came from; a dummy waits for fresh ones.
static void forgetRemotePositions(std::map<uint32_t, Client>& clients) {
    for (auto& [id, client] : clients) {
        if (!client.self) {
            client.hasPlayerUpdate = false;
        }
    }
}

// The stage's flags are now live in dSv_info_c::mMemory.
void Session::onStageSaveTableLoaded(int saveTblNo) {
    mCurrentSaveTblNo =
        (saveTblNo >= 0 && saveTblNo < dSv_save_c::STAGE_MAX) ? saveTblNo : -1;
    sync::onStageSaveTableLoaded();
    story::onStageSaveTableLoaded();
}

// A reload of the same stage and layer (a void-out) drops the old positions here.
void Session::onStageSaveTableUnloaded() {
    mCurrentSaveTblNo = -1;
    sync::onStageSaveTableUnloaded();
    forgetRemotePositions(mClients);
}

void Session::tickStageTracking() {
    const bool saveLoaded = isSaveLoaded();
    const char* stageName = "";
    int8_t layerNo = -1;
    int8_t roomNo = -1;
    int saveTblNo = -1;
    if (saveLoaded) {
        stageName = dComIfGp_getStartStageName();
        if (!stageName || stageName[0] == '\0') {
            return;
        }
        layerNo = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
        roomNo = static_cast<int8_t>(dStage_roomControl_c::getStayNo());
        saveTblNo = mCurrentSaveTblNo;
    }

    if (mLastSaveLoaded == saveLoaded &&
        std::strncmp(mLastStageName, stageName, sizeof(mLastStageName)) == 0 &&
        mLastLayerNo == layerNo &&
        mLastRoomNo == roomNo &&
        mLastSaveTblNo == saveTblNo)
    {
        return;
    }

    if (mLastLayerNo != layerNo ||
        std::strncmp(mLastStageName, stageName, sizeof(mLastStageName)) != 0)
    {
        forgetRemotePositions(mClients);
    }

    if (mLastLayerNo != layerNo ||
        std::strncmp(mLastStageName, stageName, sizeof(mLastStageName)) != 0)
    {
        mSceneChangedAt = std::chrono::steady_clock::now();
    }
    std::memset(mLastStageName, 0, sizeof(mLastStageName));
    std::strncpy(mLastStageName, stageName, sizeof(mLastStageName) - 1);
    mLastLayerNo = layerNo;
    mLastRoomNo = roomNo;
    mLastSaveTblNo = saveTblNo;
    mLastSaveLoaded = saveLoaded;
    TwiliLog.info("[session] stage={} layer={} room={} saveTbl={} saveLoaded={}",
                 stageName, layerNo, roomNo, saveTblNo, saveLoaded);
    send({
        {"type", "UPDATE_CLIENT_STATE"},
        {"stageName", std::string(stageName)},
        {"layerNo", static_cast<int>(layerNo)},
        {"roomNo", static_cast<int>(roomNo)},
        {"saveTblNo", saveTblNo},
        {"isSaveLoaded", saveLoaded},
    });
    syncSelfRow();
}

std::array<int, 3> localPlayerColor() {
    std::string text = config::getString(config::Var::Color);
    if (!text.empty() && text[0] == '#') {
        text.erase(0, 1);
    }
    const auto hex = [](char ch) {
        return ch >= '0' && ch <= '9'   ? ch - '0' :
               ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
               ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 :
                                        -1;
    };
    std::array<int, 3> color{255, 255, 255};
    if (text.size() != 6 && text.size() != 8) {
        return color;
    }
    for (size_t i = 0; i < 3; ++i) {
        const int high = hex(text[i * 2]);
        const int low = hex(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return {255, 255, 255};
        }
        color[i] = high * 16 + low;
    }
    return color;
}

void Session::rememberSentColor(const std::array<int, 3>& color) {
    mSentColor = color;
    mSentColorAt = std::chrono::steady_clock::now();
}

void Session::tickSelfColor() {
    if (mSelfClientId == 0) {
        return;
    }
    const auto color = localPlayerColor();
    if (color == mSentColor ||
        std::chrono::steady_clock::now() - mSentColorAt < kColorPushInterval)
    {
        return;
    }
    send({
        {"type", "UPDATE_CLIENT_STATE"},
        {"color", {{"r", color[0]}, {"g", color[1]}, {"b", color[2]}}},
    });
    rememberSentColor(color);
    ++mColorPushCount;
    syncSelfRow();
}

void Session::syncSelfRow() {
    const auto it = mClients.find(mSelfClientId);
    if (mSelfClientId == 0 || it == mClients.end()) {
        return;
    }
    Client& self = it->second;
    if (mLastLayerNo != -127) {
        self.isSaveLoaded = mLastSaveLoaded;
        copyStageName(self, mLastStageName);
        self.layerNo = mLastLayerNo;
        self.roomNo = mLastRoomNo;
        self.saveTblNo = static_cast<int8_t>(mLastSaveTblNo);
    }
    if (mSentColor[0] >= 0) {
        self.colorR = static_cast<uint8_t>(mSentColor[0]);
        self.colorG = static_cast<uint8_t>(mSentColor[1]);
        self.colorB = static_cast<uint8_t>(mSentColor[2]);
    }
}

void Session::handleAllClientState(const nlohmann::json& packet) {
    mClients.clear();
    const auto arr = packet.find("clients");
    const auto stateArr = packet.find("state");
    const nlohmann::json* clientsJson = nullptr;
    if (arr != packet.end() && arr->is_array()) {
        clientsJson = &*arr;
    } else if (stateArr != packet.end() && stateArr->is_array()) {
        clientsJson = &*stateArr;
    }
    if (!clientsJson) return;

    for (const auto& c : *clientsJson) {
        Client client;
        applyClientState(client, c);
        if (client.clientId == 0) continue;
        if (client.self) {
            mSelfClientId = client.clientId;
        }
        mClients[client.clientId] = std::move(client);
    }

    if (const auto room = packet.find("roomState"); room != packet.end() && room->is_object()) {
        mRoomState.applyJson(*room);
    }

    if (mSelfClientId != 0) {
        TwiliLog.info("[session] joined as client {} ({} other client(s), room owner {})",
                     mSelfClientId, mClients.size() - 1, mRoomState.ownerClientId);
        syncSelfRow();
        onSelfStateKnown();
    }
}

void Session::handleUpdateClientState(const nlohmann::json& packet) {
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0) return;
    const auto& state = clientStatePayload(packet);

    if (!state.value("online", true)) {
        mClients.erase(id);
        return;
    }

    auto& c = mClients[id];
    std::array<char, sizeof(c.stageName)> oldStageName;
    std::memcpy(oldStageName.data(), c.stageName, oldStageName.size());
    const int8_t oldLayerNo = c.layerNo;
    const bool oldSaveLoaded = c.isSaveLoaded;

    c.clientId = id;
    c.online = true;
    applyClientState(c, state);
    c.clientId = id;
    c.online = true;

    const bool sceneChanged =
        oldLayerNo != c.layerNo ||
        std::strncmp(oldStageName.data(), c.stageName, sizeof(c.stageName)) != 0;
    if (!c.isSaveLoaded || !oldSaveLoaded || sceneChanged) {
        c.hasPlayerUpdate = false;
    }
}

}  // namespace twili
