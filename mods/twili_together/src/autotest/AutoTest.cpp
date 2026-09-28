#include "autotest/AutoTest.hpp"
#include "autotest/AutoTestSteps.hpp"
#include "autotest/State.hpp"

#include "core/Config.hpp"
#include "core/Host.hpp"
#include "core/Log.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_meter2_info.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace twili::autotest {
namespace detail {
namespace {

using nlohmann::json;

// Player present and no stage change pending for this long (covers the fade-in).
constexpr int kStableStageTicks = 60;

State s;
NetDriver* s_net = nullptr;

const char* currentStage() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr ? stage : "";
}

void writeResult(bool pass, const std::string& reason) {
    if (s.resultPath.empty()) {
        return;
    }
    const json result = {
        {"instance", s.instance},
        {"pass", pass},
        {"reason", reason},
        {"exitCode", s.exitCode},
        {"step", s.stepIndex},
        {"stepCount", s.steps.size()},
        {"elapsedSec", secondsSince(s.startedAt)},
        {"frames", s.simTicks},
        {"ticks", s.simTicks},
        {"stage", currentStage()},
        {"maxPeers", s.maxPeers},
        {"maxDummies", s.maxDummies},
        {"warps", s.warps},
    };
    std::ofstream out(s.resultPath, std::ios::trunc);
    out << result.dump(2) << '\n';
}

bool stageIsStable(const std::string& wanted) {
    const bool loaded = dComIfGp_getPlayer(0) != nullptr && !dComIfGp_isEnableNextStage() &&
                        (wanted.empty() || wanted == currentStage());
    if (loaded && !s.stageLoaded) {
        s.stageLoadedTick = s.simTicks;
    }
    s.stageLoaded = loaded;
    return loaded && s.simTicks - s.stageLoadedTick >= kStableStageTicks;
}

int stepTicks() {
    return static_cast<int>(s.simTicks - s.stepStartTick);
}

// Function-local so other files' static initializers can register.
std::vector<StepHandler>& stepHandlers() {
    static std::vector<StepHandler> handlers;
    return handlers;
}

double stepTimeout(const json& step, double fallback = 90.0) {
    return step.value("timeoutSec", fallback);
}

bool timedOut(const json& step, const char* what) {
    if (secondsSince(s.stepStartedAt) > stepTimeout(step)) {
        fail(fmt::format("{} timed out after {:.0f}s", what, stepTimeout(step)));
        return true;
    }
    return false;
}

NetDriver* net(const std::string& op) {
    if (s_net == nullptr) {
        fail(fmt::format("'{}' needs the network session, which is not running", op));
    }
    return s_net;
}

bool runStep(const json& step) {
    const std::string op = step.value("op", std::string{});

    if (op == "log") {
        TwiliLog.info("[autotest] {}", step.value("msg", std::string{}));
        return true;
    }

    if (op == "wait") {
        if (step.contains("frames")) {
            return stepTicks() >= step.value("frames", 0);
        }
        return secondsSince(s.stepStartedAt) >= step.value("sec", 1.0);
    }

    if (op == "waitStage") {
        const std::string stage = step.value("stage", std::string{});
        if (stageIsStable(stage)) {
            TwiliLog.info("[autotest] in stage {} room {} layer {}", currentStage(),
                dStage_roomControl_c::getStayNo(), dComIfG_play_c::getLayerNo(0));
            return true;
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step)) {
            fail(fmt::format("waitStage {} timed out after {:.0f}s (stage='{}' player={} "
                             "nextStagePending={} eventRunning={} ticks={})",
                stage, stepTimeout(step), currentStage(), dComIfGp_getPlayer(0) != nullptr,
                dComIfGp_isEnableNextStage() != FALSE, dComIfGp_event_runCheck() != FALSE,
                s.simTicks));
        }
        return false;
    }

    if (op == "warp") {
        if (!s.stepBegun) {
            const std::string stage = step.value("stage", std::string{});
            if (stage.empty() || stage.size() > 7) {
                fail("warp needs a stage name of 1-7 characters");
                return false;
            }
            dComIfGp_setNextStage(stage.c_str(), static_cast<s16>(step.value("point", 0)),
                static_cast<s8>(step.value("room", 0)), static_cast<s8>(step.value("layer", -1)),
                0.0f, 0, 1, 0, 0, 1, 3);
            s.warps++;
            s.stageLoaded = false;
        }
        return true;
    }

    if (op == "walk") {
        if (!s.stepBegun) {
            s.padActive = true;
            s.padFrame = 0;
            s.padEndTick = s.simTicks + step.value("frames", 120);
            s.padStickX = step.value("stickX", 0.0f);
            s.padStickY = step.value("stickY", 1.0f);
            s.padCircle = step.value("circle", false);
            s.padButtons = static_cast<uint16_t>(step.value("buttons", 0));
        }
        return step.value("async", false) || !s.padActive;
    }

    if (op == "markPos" || op == "expectPos") {
        const fopAc_ac_c* player = dComIfGp_getPlayer(0);
        if (player == nullptr) {
            fail(op + ": no player actor");
            return false;
        }
        const cXyz& pos = player->current.pos;
        if (op == "markPos") {
            s.hasMark = true;
            s.markX = pos.x;
            s.markY = pos.y;
            s.markZ = pos.z;
            TwiliLog.info(
                "[autotest] marked position ({:.0f}, {:.0f}, {:.0f})", pos.x, pos.y, pos.z);
            return true;
        }
        const auto distance = [&](float x, float y, float z) {
            const float dx = pos.x - x;
            const float dy = pos.y - y;
            const float dz = pos.z - z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        float moved = -1.0f;
        if (s.hasMark) {
            moved = distance(s.markX, s.markY, s.markZ);
        }
        TwiliLog.info("[autotest] position ({:.0f}, {:.0f}, {:.0f}), moved {:.0f}", pos.x, pos.y,
            pos.z, moved);
        if (step.contains("minMoved")) {
            const float want = step.value("minMoved", 0.0f);
            if (!s.hasMark) {
                fail("expectPos minMoved needs a markPos first");
                return false;
            }
            if (moved < want) {
                fail(fmt::format(
                    "player moved {:.0f} units from the mark, want at least {:.0f}", moved, want));
                return false;
            }
        }
        if (const auto near = step.find("near"); near != step.end()) {
            if (!near->is_array() || near->size() != 3) {
                fail("expectPos near must be [x, y, z]");
                return false;
            }
            const float maxDist = step.value("maxDist", 100.0f);
            const float dist =
                distance((*near)[0].get<float>(), (*near)[1].get<float>(), (*near)[2].get<float>());
            if (dist > maxDist) {
                fail(fmt::format("player is {:.0f} units from the expected position (max {:.0f})",
                    dist, maxDist));
                return false;
            }
        }
        return true;
    }

    if (op == "signal") {
        if (auto* driver = net(op)) {
            driver->sendSignal(s.instance, step.value("name", std::string{}));
            return true;
        }
        return false;
    }

    if (op == "waitSignal") {
        const std::string name = step.value("name", std::string{});
        const std::string from = step.value("from", std::string{});
        if (s.signals.count(from.empty() ? name : from + ":" + name)) {
            return true;
        }
        timedOut(step, "waitSignal");
        return false;
    }

    // set* steps use the gameplay accessors so the sync hooks see them.
    if (op == "giveItem") {
        execItemGet(static_cast<u8>(step.value("item", 0)));
        return true;
    }

    if (op == "expectItem") {
        const int item = step.value("item", 0);
        if (dComIfGs_isItemFirstBit(static_cast<u8>(item))) {
            return true;
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step, 30.0)) {
            fail(fmt::format("item 0x{:02X} never arrived", item));
        }
        return false;
    }

    // Small keys of the current stage.
    if (op == "addKeys") {
        dSv_memBit_c& bit = dComIfGs_getSaveInfo()->getMemory().getBit();
        bit.mKeyNum = static_cast<u8>(std::clamp(bit.mKeyNum + step.value("count", 1), 0, 255));
        return true;
    }

    if (op == "expectKeys") {
        // forSec: must also hold that long (no late duplicate).
        static bool sReached = false;
        if (!s.stepBegun) {
            sReached = false;
        }
        const int want = step.value("count", 0);
        const int have = dComIfGs_getSaveInfo()->getMemory().getBit().mKeyNum;
        const double hold = step.value("forSec", 0.0);
        if (have != want && sReached) {
            fail(fmt::format("small keys changed to {} (want {})", have, want));
            return false;
        }
        if (have == want) {
            sReached = true;
            return secondsSince(s.stepStartedAt) >= hold;
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step, 20.0)) {
            fail(fmt::format("small keys {} never became {}", have, want));
        }
        return false;
    }

    // What a burning wooden shield does to the save (d_a_alink.cpp).
    if (op == "burnShield") {
        dMeter2Info_setShield(dItemNo_NONE_e, true);
        return true;
    }

    if (op == "expectShield") {
        // item 255 = none; owned also checks its first-get bit.
        static bool sReached = false;
        if (!s.stepBegun) {
            sReached = false;
        }
        const int want = step.value("item", 255);
        const int have = dComIfGs_getSelectEquipShield();
        bool ok = have == want;
        if (ok && step.contains("owned") && want != 255) {
            ok = (dComIfGs_isItemFirstBit(static_cast<u8>(want)) != 0) == step.value("owned", true);
        }
        if (!ok && sReached) {
            fail(fmt::format("shield changed to 0x{:02X} (want 0x{:02X})", have, want));
            return false;
        }
        if (ok) {
            sReached = true;
            return secondsSince(s.stepStartedAt) >= step.value("forSec", 0.0);
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step, 20.0)) {
            fail(fmt::format("shield 0x{:02X} never became 0x{:02X}", have, want));
        }
        return false;
    }

    if (op == "setSwitch" || op == "unsetSwitch") {
        const int no = step.value("no", 0);
        const int room = step.value("room", static_cast<int>(dStage_roomControl_c::getStayNo()));
        if (op == "setSwitch") {
            dComIfGs_onSwitch(no, room);
        } else {
            dComIfGs_offSwitch(no, room);
        }
        return true;
    }

    if (op == "expectSwitch") {
        const int no = step.value("no", 0);
        const int room = step.value("room", static_cast<int>(dStage_roomControl_c::getStayNo()));
        const bool want = step.value("set", true);
        if ((dComIfGs_isSwitch(no, room) != FALSE) == want) {
            return true;
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step, 30.0)) {
            fail(fmt::format(
                "switch {} (room {}) never became {}", no, room, want ? "set" : "clear"));
        }
        return false;
    }

    if (op == "setEventBit") {
        dComIfGs_onEventBit(static_cast<u16>(step.value("no", 0)));
        return true;
    }

    if (op == "expectEventBit") {
        const int no = step.value("no", 0);
        const bool want = step.value("set", true);
        if ((dComIfGs_isEventBit(static_cast<u16>(no)) != FALSE) == want) {
            return true;
        }
        if (secondsSince(s.stepStartedAt) > stepTimeout(step, 30.0)) {
            fail(fmt::format("event bit 0x{:04X} never became {}", no, want ? "set" : "clear"));
        }
        return false;
    }

    if (op == "fail") {
        fail(step.value("reason", std::string("scripted failure")));
        return false;
    }

    if (op == "quit") {
        finish(true, kExitPass, "script complete");
        return false;
    }

    if (op == "exitNow") {
        // Like a crash: no disconnect, no teardown.
        TwiliLog.info("[autotest] RESULT PASS step={}/{} reason=\"exitNow\" elapsed={:.1f}s",
            s.stepIndex, s.steps.size(), secondsSince(s.startedAt));
        s.exitCode = kExitPass;
        writeResult(true, "exitNow");
        std::_Exit(kExitPass);
    }

    if (op == "connect") {
        if (auto* driver = net(op)) {
            if (!s.stepBegun) {
                driver->connect(s.url, step.value("name", s.instance), step.value("room", s.room),
                    step.value("team", s.team));
            }
            return true;
        }
        return false;
    }

    if (op == "disconnect") {
        if (auto* driver = net(op)) {
            driver->disconnect();
            return true;
        }
        return false;
    }

    if (op == "waitConnected") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        if (driver->connected() && driver->selfClientId() != 0) {
            TwiliLog.info("[autotest] connected as client {}", driver->selfClientId());
            return true;
        }
        if (const std::string failure = driver->failureMessage(); !failure.empty()) {
            fail("connect failed: " + failure);
            return false;
        }
        timedOut(step, "waitConnected");
        return false;
    }

    if (op == "waitPeers") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        const int want = step.value("count", 1);
        const int have = driver->countPeers(step.value("sameStage", false));
        if (have >= want) {
            TwiliLog.info("[autotest] {} peer(s) present", have);
            return true;
        }
        timedOut(step, "waitPeers");
        return false;
    }

    if (op == "waitDummies") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        const int want = step.value("count", 1);
        if (driver->countDummies() >= want) {
            TwiliLog.info("[autotest] {} dummy player(s) spawned", driver->countDummies());
            return true;
        }
        timedOut(step, "waitDummies");
        return false;
    }

    if (op == "waitNoDummies") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        if (driver->countDummies() == 0) {
            return true;
        }
        timedOut(step, "waitNoDummies");
        return false;
    }

    if (op == "checkDummies") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        std::string why;
        if (!driver->checkDummies(step.value("maxDist", 300.0f), why)) {
            fail("checkDummies: " + why);
            return false;
        }
        return true;
    }

    if (op == "expectPeers") {
        auto* driver = net(op);
        if (driver == nullptr) {
            return false;
        }
        const int want = step.value("count", 1);
        const int have = driver->countPeers(step.value("sameStage", false));
        if (have != want) {
            fail(fmt::format("expected {} peer(s), have {}", want, have));
            return false;
        }
        return true;
    }

    StepContext ctx{step, s.stepBegun, stepTicks(), secondsSince(s.stepStartedAt)};
    for (StepHandler handler : stepHandlers()) {
        if (const std::optional<bool> done = handler(op, ctx)) {
            return *done;
        }
    }

    fail("unknown op '" + op + "'");
    return false;
}

void updateStats() {
    if (s_net != nullptr && s_net->connected()) {
        s.maxPeers = std::max(s.maxPeers, s_net->countPeers(false));
        s.maxDummies = std::max(s.maxDummies, s_net->countDummies());
    }
}

}  // namespace

State& state() {
    return s;
}

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

void finish(bool pass, int code, const std::string& reason) {
    if (s.finished) {
        return;
    }
    s.finished = true;
    s.exitCode = code;
    s.padActive = false;
    TwiliLog.info("[autotest] RESULT {} step={}/{} reason=\"{}\" elapsed={:.1f}s",
        pass ? "PASS" : "FAIL", s.stepIndex, s.steps.size(), reason, secondsSince(s.startedAt));
    writeResult(pass, reason);
    if (s_net != nullptr) {
        s_net->disconnect();
    }
    if (!host::requestQuit(code)) {
        TwiliLog.warn("[autotest] host has no HostService.request_quit; the runner stops the game");
    }
}

void fail(const std::string& reason) {
    finish(false, kExitFail, reason);
}

}  // namespace detail

using namespace detail;

void StepContext::fail(const std::string& reason) const {
    detail::fail(reason);
}

bool registerSteps(StepHandler handler) {
    stepHandlers().push_back(handler);
    return true;
}

bool init() {
    auto& st = state();
    const std::string scriptPath = config::getString(config::Var::AutotestScript);
    if (scriptPath.empty()) {
        return true;
    }

    st.active = true;
    st.scriptPath = scriptPath;
    st.startedAt = Clock::now();
    st.stepStartedAt = st.startedAt;
    suppressErrorDialogs();

    try {
        std::ifstream in(scriptPath);
        if (!in) {
            st.initFailure = "cannot open script " + scriptPath;
        } else {
            const nlohmann::json script = nlohmann::json::parse(in);
            st.instance = script.value("instance", std::string("autotest"));
            st.url = script.value("url", std::string("ws://127.0.0.1:3000"));
            st.room = script.value("room", std::string("autotest"));
            st.team = script.value("team", std::string{});
            st.resultPath = script.value("resultPath", std::string{});
            st.timeoutSec = script.value("timeoutSec", 240.0);
            st.start = script.value("start", nlohmann::json::object());
            st.steps = script.value("steps", nlohmann::json::array());
        }
    } catch (const std::exception& e) {
        st.initFailure = fmt::format("bad script {}: {}", scriptPath, e.what());
    }

    // Mods cannot override host settings, so the runner's config must have these off.
    if (st.initFailure.empty() && config::hostBool("game.autoSave", false)) {
        st.initFailure = "test config has game.autoSave on";
    }
    if (st.initFailure.empty() && config::hostBool("game.pauseOnFocusLost", false)) {
        st.initFailure = "test config has game.pauseOnFocusLost on";
    }

    if (!st.initFailure.empty()) {
        TwiliLog.error("[autotest] {}", st.initFailure);
        return false;
    }
    TwiliLog.info("[autotest] instance={} script={} steps={} timeout={}s", st.instance, scriptPath,
        st.steps.size(), st.timeoutSec);
    return true;
}

bool isActive() {
    return state().active;
}

uint64_t ticks() {
    return state().simTicks;
}

void tick() {
    auto& st = state();
    if (!st.active || st.finished) {
        return;
    }
    st.simTicks++;
    if (!st.initFailure.empty()) {
        fail(st.initFailure);
        return;
    }
    updateStats();

    if (secondsSince(st.startedAt) > st.timeoutSec) {
        finish(false, kExitTimeout,
            fmt::format("global timeout ({:.0f}s) during step {}", st.timeoutSec, st.stepIndex));
        return;
    }

    if (st.stepIndex >= st.steps.size()) {
        finish(true, kExitPass, "script complete");
        return;
    }

    const nlohmann::json& step = st.steps[st.stepIndex];
    if (!st.stepBegun) {
        st.stepStartedAt = Clock::now();
        st.stepStartTick = st.simTicks;
        st.stageLoaded = false;
        TwiliLog.info("[autotest] step {}/{} {}", st.stepIndex + 1, st.steps.size(), step.dump());
    }

    bool done = false;
    try {
        done = runStep(step);
    } catch (const std::exception& e) {
        fail(fmt::format("step threw: {}", e.what()));
        return;
    }
    st.stepBegun = true;

    if (done && !st.finished) {
        st.stepIndex++;
        st.stepBegun = false;
    }
}

void setNetDriver(NetDriver* driver) {
    s_net = driver;
}

void onSignal(const std::string& instance, const std::string& name) {
    auto& st = state();
    st.signals.insert(name);
    st.signals.insert(instance + ":" + name);
    TwiliLog.info("[autotest] signal '{}' from {}", name, instance);
}

}  // namespace twili::autotest
