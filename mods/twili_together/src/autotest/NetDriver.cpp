#include "autotest/AutoTest.hpp"

#include "core/Session.hpp"

#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <cmath>
#include <cstring>

namespace twili::autotest {
namespace {

const char* currentStage() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr ? stage : "";
}

bool clientInMyLayer(const Client& c) {
    return c.online && c.isSaveLoaded &&
           std::strncmp(c.stageName, currentStage(), sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

// The autotest's view of the session.
class SessionNetDriver final : public NetDriver {
public:
    void connect(const std::string& url, const std::string& name, const std::string& room,
        const std::string& team) override {
        Session::instance().connect({url, name, room, team});
    }

    void disconnect() override { Session::instance().disconnect(); }

    bool connected() const override { return Session::instance().joined(); }

    uint32_t selfClientId() const override { return Session::instance().selfClientId(); }

    std::string failureMessage() const override {
        return Session::instance().connectionFailureMessage();
    }

    int countPeers(bool sameStage) const override {
        int n = 0;
        for (const auto& [id, c] : Session::instance().clients()) {
            if (c.self || !c.online || (sameStage && !clientInMyLayer(c))) {
                continue;
            }
            n++;
        }
        return n;
    }

    int countDummies() const override {
        const Session& session = Session::instance();
        int n = 0;
        for (const auto& [id, c] : session.clients()) {
            if (!c.self && session.dummyActorForClient(id) != nullptr) {
                n++;
            }
        }
        return n;
    }

    // Every peer in our layer that sends poses has a dummy near its reported position.
    bool checkDummies(float maxDist, std::string& why) const override {
        const Session& session = Session::instance();
        for (const auto& [id, c] : session.clients()) {
            if (c.self || !clientInMyLayer(c) || !c.hasPlayerUpdate) {
                continue;
            }
            fopAc_ac_c* dummy = session.dummyActorForClient(id);
            if (dummy == nullptr) {
                why = fmt::format("no dummy for client {} ({})", id, c.name);
                return false;
            }
            const float dx = dummy->current.pos.x - c.posX;
            const float dy = dummy->current.pos.y - c.posY;
            const float dz = dummy->current.pos.z - c.posZ;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > maxDist) {
                why = fmt::format(
                    "dummy for client {} is {:.0f} units from its reported position", id, dist);
                return false;
            }
        }
        return true;
    }

    void sendSignal(const std::string& instance, const std::string& name) override {
        Session::instance().sendAutotestSignal(instance, name);
    }
};

SessionNetDriver s_driver;

}  // namespace

void installNetDriver() {
    setNetDriver(&s_driver);
}

void removeNetDriver() {
    setNetDriver(nullptr);
}

}  // namespace twili::autotest
