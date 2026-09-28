#include "core/GameAccess.hpp"
#include "core/Log.hpp"
#include "core/SaveGate.hpp"
#include "core/Session.hpp"

#include "d/d_com_inf_game.h"

#include <algorithm>
#include <cstring>

namespace twili {

// How long a horse puppet outlives the last tick it was wanted.
static constexpr auto kHorseGrace = std::chrono::seconds(5);

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

fopAc_ac_c* Session::horseActorForClient(uint32_t clientId) const {
    const auto it = mHorseActors.find(clientId);
    if (it == mHorseActors.end() || !fopAcM_IsExecuting(it->second)) {
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
        manageHorsePuppets(false);
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
    manageHorsePuppets(stageReady);
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

// Deletes the puppet of `it`, or forgets it if it never executed (a create under way deletes
// itself once its client is gone).
static std::map<uint32_t, fpc_ProcID>::iterator dropActor(std::map<uint32_t, fpc_ProcID>& actors,
    std::map<uint32_t, fpc_ProcID>::iterator it, std::set<uint32_t>& seen) {
    if (fopAcM_IsExecuting(it->second)) {
        fopAcM_delete(it->second);
    } else if (fpcM_IsCreating(it->second)) {
        return std::next(it);
    }
    seen.erase(it->first);
    return actors.erase(it);
}

void Session::manageHorsePuppets(bool stageReady) {
    if (!isSaveLoaded() || mLastStageName[0] == '\0' || mLastLayerNo == -127) {
        for (auto it = mHorseActors.begin(); it != mHorseActors.end();) {
            it = dropActor(mHorseActors, it, mHorseSeenExecuting);
        }
        mHorseCreateFailures.clear();
        mHorseShownHere.clear();
        mHorseWantedAt.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto it = mHorseActors.begin(); it != mHorseActors.end();) {
        const uint32_t id = it->first;
        if (fopAcM_IsExecuting(it->second)) {
            mHorseSeenExecuting.insert(id);
            mHorseCreateFailures.erase(id);
            ++it;
        } else if (fpcM_IsCreating(it->second)) {
            ++it;
        } else {
            // A failed create (no heap for Horse.arc) backs off like the dummies'.
            if (!mHorseSeenExecuting.count(id)) {
                auto& failure = mHorseCreateFailures[id];
                failure.count++;
                const int delaySec = std::min(1 << failure.count, 30);
                failure.retryAt = now + std::chrono::seconds(delaySec);
                TwiliLog.warn("[horse] horse for client {} failed to create (attempt {}), "
                              "retrying in {}s",
                    id, failure.count, delaySec);
            }
            mHorseSeenExecuting.erase(id);
            it = mHorseActors.erase(it);
        }
    }

    // Live: its dummy is here and its horse was shown since. Parked: its owner is in another
    // stage and left the horse in ours.
    auto wanted = [this](uint32_t id, const Client& c, bool& parked) {
        parked = false;
        if (clientHasPlayerInCurrentLayer(c)) {
            if (c.horse.present()) {
                mHorseShownHere.insert(id);
            }
            return mHorseShownHere.count(id) != 0 && mDummyActors.count(id) != 0;
        }
        mHorseShownHere.erase(id);
        parked = !c.self && c.online && c.isSaveLoaded && c.horsePlace.valid &&
                 std::strncmp(c.horsePlace.stage, mLastStageName, sizeof(mLastStageName)) == 0 &&
                 std::strncmp(c.stageName, mLastStageName, sizeof(mLastStageName)) != 0;
        return parked;
    };

    for (auto it = mHorseActors.begin(); it != mHorseActors.end();) {
        const auto clientIt = mClients.find(it->first);
        bool parked = false;
        if (clientIt != mClients.end() && wanted(it->first, clientIt->second, parked)) {
            mHorseWantedAt[it->first] = now;
            ++it;
            continue;
        }
        // Its owner just changed scene: the puppet waits hidden for the horse place or the next
        // keyframe instead of loading Horse.arc again.
        if (clientIt != mClients.end() && now - mHorseWantedAt[it->first] < kHorseGrace) {
            ++it;
            continue;
        }
        mHorseShownHere.erase(it->first);
        mHorseWantedAt.erase(it->first);
        it = dropActor(mHorseActors, it, mHorseSeenExecuting);
    }

    if (!stageReady || g_procDummyHorse < 0) {
        return;
    }
    for (const auto& [id, c] : mClients) {
        bool parked = false;
        if (mHorseActors.count(id) || !wanted(id, c, parked)) {
            continue;
        }
        if (const auto failure = mHorseCreateFailures.find(id);
            failure != mHorseCreateFailures.end() && now < failure->second.retryAt)
        {
            continue;
        }
        const cXyz pos = parked ?
                             cXyz(c.horsePlace.pos[0], c.horsePlace.pos[1], c.horsePlace.pos[2]) :
                             cXyz(c.horse.pos[0], c.horse.pos[1], c.horse.pos[2]);
        const csXyz angle(0, parked ? c.horsePlace.angleY : c.horse.angle[1], 0);
        const fpc_ProcID pid =
            fopAcM_create(g_procDummyHorse, static_cast<u32>(id), &pos, -1, &angle, nullptr, 0);
        if (pid != fpcM_ERROR_PROCESS_ID_e) {
            mHorseActors[id] = pid;
            mHorseWantedAt[id] = now;
            TwiliLog.info("[horse] horse for client {} ({})", id, parked ? "parked" : "live");
        }
    }
}

// Horses still being created delete themselves once their client is gone.
void Session::destroyHorsePuppets() {
    for (const auto& [id, pid] : mHorseActors) {
        if (fopAcM_IsExecuting(pid)) {
            fopAcM_delete(pid);
        }
    }
    mHorseActors.clear();
    mHorseSeenExecuting.clear();
    mHorseCreateFailures.clear();
    mHorseShownHere.clear();
    mHorseWantedAt.clear();
}

}  // namespace twili
