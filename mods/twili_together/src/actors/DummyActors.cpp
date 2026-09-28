#include "core/GameAccess.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"

#include "d/d_com_inf_game.h"

#include <algorithm>
#include <cstring>

namespace twili {

bool Session::clientIsInCurrentLayer(const Client& client) const {
    if (client.self || !client.online || !client.isSaveLoaded) {
        return false;
    }
    if (mLastStageName[0] == '\0' || mLastLayerNo == -127) {
        return false;
    }
    return client.layerNo == mLastLayerNo &&
           std::strncmp(client.stageName, mLastStageName, sizeof(client.stageName)) == 0;
}

bool Session::clientHasPlayerInCurrentLayer(const Client& client) const {
    return client.hasPlayerUpdate && clientIsInCurrentLayer(client);
}

bool Session::hasRemoteClientInCurrentLayer() const {
    return std::any_of(mClients.begin(), mClients.end(),
        [this](const auto& entry) { return clientIsInCurrentLayer(entry.second); });
}

fopAc_ac_c* Session::dummyActorForClient(uint32_t clientId) const {
    const auto it = mDummyActors.find(clientId);
    if (it == mDummyActors.end() || !fopAcM_IsExecuting(it->second)) {
        return nullptr;
    }
    fopAc_ac_c* actor = nullptr;
    if (!fopAcM_SearchByID(it->second, &actor) || actor == nullptr) {
        return nullptr;
    }
    return actor;
}

void Session::manageDummyActors() {
    if (!isSaveLoaded() || mLastStageName[0] == '\0' || mLastLayerNo == -127) {
        for (auto& [id, client] : mClients) {
            if (!client.self) {
                client.hasPlayerUpdate = false;
            }
        }
        for (auto it = mDummyActors.begin(); it != mDummyActors.end();) {
            if (fopAcM_IsExecuting(it->second)) {
                fopAcM_delete(it->second);
                mDummySeenExecuting.erase(it->first);
                it = mDummyActors.erase(it);
            } else if (!fpcM_IsCreating(it->second)) {
                mDummySeenExecuting.erase(it->first);
                it = mDummyActors.erase(it);
            } else {
                ++it;
            }
        }
        // Between stages: the next stage gets a fresh set of create attempts.
        mDummyCreateFailures.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto it = mDummyActors.begin(); it != mDummyActors.end();) {
        const uint32_t id = it->first;
        if (fopAcM_IsExecuting(it->second)) {
            mDummySeenExecuting.insert(id);
            mDummyCreateFailures.erase(id);
            ++it;
        } else if (fpcM_IsCreating(it->second)) {
            ++it;
        } else {
            // Gone before it ever executed: create() failed, so back off instead of thrashing.
            if (!mDummySeenExecuting.count(id)) {
                auto& failure = mDummyCreateFailures[id];
                failure.count++;
                const int delaySec = std::min(1 << failure.count, 30);
                failure.retryAt = now + std::chrono::seconds(delaySec);
                TwiliLog.warn("[dummy] dummy for client {} failed to create (attempt {}), "
                              "retrying in {}s",
                    id, failure.count, delaySec);
            }
            mDummySeenExecuting.erase(id);
            it = mDummyActors.erase(it);
        }
    }

    for (auto it = mDummyActors.begin(); it != mDummyActors.end();) {
        const auto clientIt = mClients.find(it->first);
        if (clientIt != mClients.end() && clientHasPlayerInCurrentLayer(clientIt->second)) {
            ++it;
            continue;
        }
        if (fopAcM_IsExecuting(it->second)) {
            fopAcM_delete(it->second);
            mDummySeenExecuting.erase(it->first);
            it = mDummyActors.erase(it);
        } else if (!fpcM_IsCreating(it->second)) {
            mDummySeenExecuting.erase(it->first);
            it = mDummyActors.erase(it);
        } else {
            ++it;
        }
    }

    // The dummy's create reads the local player, which exists only once the stage has loaded.
    const bool stageReady = mCurrentSaveTblNo >= 0 && dComIfGp_getPlayer(0) != nullptr &&
                            !dComIfGp_isEnableNextStage();
    if (!stageReady || g_procDummyPlayer < 0) {
        return;
    }

    for (const auto& [id, c] : mClients) {
        if (!clientHasPlayerInCurrentLayer(c) || mDummyActors.count(id)) {
            continue;
        }
        if (const auto failure = mDummyCreateFailures.find(id);
            failure != mDummyCreateFailures.end() && now < failure->second.retryAt)
        {
            continue;
        }
        const cXyz pos(c.posX, c.posY, c.posZ);
        const csXyz angle(c.angleX, c.angleY, c.angleZ);
        const fpc_ProcID pid = fopAcM_create(
            g_procDummyPlayer, static_cast<u32>(id), &pos, -1, &angle, nullptr, 0);
        if (pid != fpcM_ERROR_PROCESS_ID_e) {
            mDummyActors[id] = pid;
        }
    }
}

}  // namespace twili
