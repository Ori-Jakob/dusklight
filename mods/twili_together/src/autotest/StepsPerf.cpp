// Step for the cost of our hooks; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Log.hpp"
#include "hooks/Hooks.hpp"
#include "hooks/Perf.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace twili::autotest {

// A getter the game calls only in a debug view; the game's copy of the header-inline wrapper.
#ifdef _MSVC_LANG
#define perfProbe_sig "?dComIfGp_particle_getHeapSize@@YAIXZ"
#else
#define perfProbe_sig "_Z29dComIfGp_particle_getHeapSizev"
#endif
DEFINE_HOOK_SYMBOL(perfProbe_sig, uint32_t(), PerfProbe);

namespace {

using hooks::perf::Target;

bool sProbeInstalled = false;
std::array<uint32_t, static_cast<size_t>(Target::Count)> sStartCalls{};
double sDispatchNs = 0.0;

void onProbePost(ModContext*, void*, void*, void*) {}

double nsPerCall(uint32_t (*fn)(), int calls) {
    volatile int sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; i++) {
        sink += fn();
    }
    const auto end = std::chrono::steady_clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(end - start).count() / calls;
}

// One dispatch through the host with one post callback, less the call itself.
double measureDispatchNs() {
    constexpr int kCalls = 20000;
    double best = 1e9;
    for (int round = 0; round < 5; round++) {
        const double hooked = nsPerCall(reinterpret_cast<uint32_t (*)()>(PerfProbe::target), kCalls);
        const double direct = nsPerCall(PerfProbe::g_orig, kCalls);
        best = std::min(best, hooked - direct);
    }
    return std::max(best, 0.0);
}

bool measureHookCost(StepContext& ctx) {
    const int frames = std::max(1, ctx.step.value("frames", 300));
    if (!ctx.begun) {
        if (!sProbeInstalled) {
            std::string error;
            if (hooks::addPost<PerfProbe>(onProbePost, hooks::kDefault, "perf probe", error) !=
                MOD_OK)
            {
                ctx.fail("measureHookCost: " + error);
                return false;
            }
            sProbeInstalled = true;
        }
        sDispatchNs = measureDispatchNs();
        if (SERVICE_HAS(svc_hook, HookService, uninstall) &&
            svc_hook->uninstall(mod_ctx, PerfProbe::target,
                reinterpret_cast<void**>(&PerfProbe::g_orig)) == MOD_OK)
        {
            sProbeInstalled = false;
        }
        sStartCalls = hooks::perf::g_calls;
        return false;
    }
    if (ctx.ticks < frames) {
        return false;
    }
    uint64_t total = 0;
    std::string detail;
    for (size_t i = 0; i < sStartCalls.size(); i++) {
        const uint32_t calls = hooks::perf::g_calls[i] - sStartCalls[i];
        total += calls;
        if (calls != 0) {
            detail += fmt::format(" {}={:.1f}", hooks::perf::targetName(static_cast<Target>(i)),
                static_cast<double>(calls) / ctx.ticks);
        }
    }
    const double perTick = static_cast<double>(total) / ctx.ticks;
    const double ms = perTick * sDispatchNs / 1e6;
    const double maxMs = ctx.step.value("maxMs", 0.1);
    TwiliLog.info("[autotest] hook cost: {:.1f} calls/tick x {:.0f} ns = {:.4f} ms/tick (max {}) "
                  "over {} ticks;{}",
        perTick, sDispatchNs, ms, maxMs, ctx.ticks, detail);
    if (ms > maxMs) {
        ctx.fail(fmt::format("measureHookCost: {:.4f} ms per tick", ms));
    }
    return true;
}

std::optional<bool> perfSteps(const std::string& op, StepContext& ctx) {
    if (op == "measureHookCost") {
        return measureHookCost(ctx);
    }
    return std::nullopt;
}

const bool sRegistered = registerSteps(&perfSteps);

}  // namespace
}  // namespace twili::autotest
