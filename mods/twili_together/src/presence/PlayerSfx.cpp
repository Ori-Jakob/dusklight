#include "actors/DummyPlayer.hpp"
#include "core/GameAccess.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace twili {

void Session::sendPlayerSfx(uint32_t soundId, PlayerSfxKind kind, uint32_t mapInfo) {
    if (!isConnected() || !isSaveLoaded() || !hasRemoteClientInCurrentLayer()) {
        return;
    }
    send(
        {
            {"type", "PLAYER_SFX"},
            {"soundId", soundId},
            {"kind", static_cast<unsigned>(kind)},
            {"mapInfo", mapInfo},
            {"quiet", true},
        },
        net::Delivery::Droppable);
}

void Session::handlePlayerSfx(const nlohmann::json& packet) {
    const uint32_t id = packet.value("clientId", 0u);
    if (id == 0 || id == mSelfClientId) {
        return;
    }

    fopAc_ac_c* actor = dummyActorForClient(id);
    if (!isDummyPlayer(actor)) {
        return;
    }

    const uint32_t soundId = packet.value("soundId", 0u);
    const uint8_t kind = static_cast<uint8_t>(packet.value("kind", 0u));
    const uint32_t mapInfo = packet.value("mapInfo", 0u);
    static_cast<daDummyPlayer_c*>(actor)->playRemotePlayerSfx(soundId, kind, mapInfo);
}

}  // namespace twili
