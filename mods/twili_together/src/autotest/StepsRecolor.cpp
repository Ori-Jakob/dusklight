// Steps for the remote player recolour (fx/PlayerRecolor.cpp).
//
// recolorSelfTest
//     The recolour maths: block modes, identity outside the band, golden vectors, the mane tint.
// expectDummyRecolor  set (kokiri|zora|magic|ordon|wolf), rgb | peerColor | vanilla, name,
//                     hueTol (20), minSat (0.25), c1Max (3), c1Min (0), maxBytes (0), frames (0),
//                     timeoutSec (20)
//     The first peer's dummy (or `name`'s) wears `set` in that colour: untouched blocks intact,
//     vanilla -> no replacement, a colour -> mean hue of its tunic ends within hueTol, a grey ->
//     saturation at most 0.12; C1 within [c1Min, c1Max]; stores within maxBytes.
// markRecolor  name
// expectRecolorDelta  name, minApplies, maxApplies, minDraws, maxGpuGrowthMB, maxPrivateGrowthMB
//     Since markRecolor; GPU memory is the process's video memory on every adapter (DXGI).
// colorCycle  count (200), everyTicks (8), s (0.8), v (0.85)
//     Changes our colour `count` times by the golden angle, like the picker.
// setRupees  value (0), timeoutSec (5)
//     In Magic Armor that drains, done once execute bound its BRK: power_down at 0, power_up else.
// recolorCamera  target (dummy|self), dist (150), height (75), up (25), side (0)
//     Our camera close on the first peer's dummy (or our Link), for window captures.

#include "autotest/AutoTestSteps.hpp"

#include "actors/DummyPlayer.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "fx/PlayerRecolor.hpp"
#include "ui/ColorMath.hpp"

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#endif

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace color = ui::color;

bool peerInMyLayer(const Client& c) {
    const char* stage = dComIfGp_getStartStageName();
    return !c.self && c.online && c.isSaveLoaded && c.hasPlayerUpdate && stage != nullptr &&
           std::strncmp(c.stageName, stage, sizeof(c.stageName)) == 0 &&
           c.layerNo == static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
}

fopAc_ac_c* peerDummy(const std::string& name, const Client** client = nullptr) {
    auto& session = Session::instance();
    for (const auto& [id, c] : session.clients()) {
        if (!peerInMyLayer(c) || (!name.empty() && c.name != name)) {
            continue;
        }
        if (fopAc_ac_c* dummy = session.dummyActorForClient(id)) {
            if (client != nullptr) *client = &c;
            return dummy;
        }
    }
    return nullptr;
}

std::string keyText(uint32_t key) {
    return key == RecolorSet::kPristine ? std::string("pristine") : fmt::format("#{:06X}", key);
}

std::string probeText(const RecolorProbe& p) {
    std::string s = fmt::format("set={} key={} applies={} {:.2f} ms c1={} bytes=0x{:X}",
        p.set >= 0 ? recolorSetName(static_cast<RecolorSetId>(p.set)) : "none",
        keyText(p.appliedKey), p.applyCount, p.lastApplyMs, p.c1MaxAbs, p.bytes);
    for (uint8_t i = 0; i < p.texCount; i++) {
        const RecolorTexProbe& t = p.tex[i];
        s += fmt::format(" [{} {} blocks h{:.0f} s{:.2f} v{:.2f}{}{}]", t.material, t.candidates,
            t.meanH, t.meanS, t.meanV, t.untouchedIntact ? "" : " UNTOUCHED-CHANGED",
            t.pristine ? " pristine" : "");
    }
    return s;
}

// What of the step's expectations the probe misses, empty when none.
std::string recolorMismatch(const json& step, const RecolorProbe& p, uint32_t wantKey,
    const Hsv& want) {
    const std::string set = step.value("set", std::string{});
    const char* worn = p.set >= 0 ? recolorSetName(static_cast<RecolorSetId>(p.set)) : "none";
    if (!set.empty() && set != worn) {
        return fmt::format("wears {}", worn);
    }
    if (!p.bound) {
        return fmt::format("{} is not bound", worn);
    }
    if (p.appliedKey != wantKey) {
        return fmt::format("textures hold {}, want {}", keyText(p.appliedKey), keyText(wantKey));
    }
    const double hueTol = step.value("hueTol", 20.0), minSat = step.value("minSat", 0.25);
    for (uint8_t i = 0; i < p.texCount; i++) {
        const RecolorTexProbe& t = p.tex[i];
        if (!t.untouchedIntact) {
            return fmt::format("{}: a block outside the recolour changed", t.material);
        }
        if (wantKey == RecolorSet::kPristine) {
            if (!t.pristine) return fmt::format("{}: not as on disc", t.material);
            continue;
        }
        if (want.s >= 0.2f) {
            if (hueDist(t.meanH, want.h) > hueTol || t.meanS < minSat) {
                return fmt::format("{}: mean h{:.0f} s{:.2f}, want h{:.0f}", t.material, t.meanH,
                    t.meanS, want.h);
            }
        } else if (want.s <= 0.02f && t.meanS > 0.12f) {
            return fmt::format("{}: mean s{:.2f} for a grey", t.material, t.meanS);
        }
    }
    if (p.c1MaxAbs > step.value("c1Max", 3) || p.c1MaxAbs < step.value("c1Min", 0)) {
        return fmt::format("C1 reads {}", p.c1MaxAbs);
    }
    const uint32_t maxBytes = step.value("maxBytes", 0u);
    if (maxBytes != 0 && p.bytes > maxBytes) {
        return fmt::format("recolour stores take 0x{:X}", p.bytes);
    }
    return {};
}

int sRecolorMatchedAt = -1;

bool expectDummyRecolor(StepContext& ctx) {
    const json& step = ctx.step;
    if (!ctx.begun) {
        sRecolorMatchedAt = -1;
    }
    const Client* client = nullptr;
    fopAc_ac_c* dummy = peerDummy(step.value("name", std::string{}), &client);
    RecolorProbe probe;
    std::string why = "no peer dummy in our layer";
    if (dummy != nullptr && GetDummyPlayerRecolorProbe(dummy, probe)) {
        std::optional<color::Rgb8> rgb;
        if (step.value("vanilla", false)) {
            rgb = color::Rgb8{255, 255, 255};
        } else if (step.value("peerColor", false)) {
            rgb = color::Rgb8{client->colorR, client->colorG, client->colorB};
        } else if (step.contains("rgb") && step["rgb"].is_array()) {
            rgb = color::Rgb8{step["rgb"][0].get<uint8_t>(), step["rgb"][1].get<uint8_t>(),
                step["rgb"][2].get<uint8_t>()};
        } else if (step.contains("rgb") && step["rgb"].is_string()) {
            rgb = color::parseHex(step["rgb"].get<std::string>());
        }
        if (!rgb) {
            ctx.fail("expectDummyRecolor: no rgb, peerColor or vanilla");
            return false;
        }
        const uint32_t key = recolorKey(rgb->r, rgb->g, rgb->b);
        const Hsv want = toHsv({rgb->r / 255.0f, rgb->g / 255.0f, rgb->b / 255.0f});
        why = recolorMismatch(step, probe, key, want);
    }
    if (why.empty()) {
        if (sRecolorMatchedAt < 0) {
            sRecolorMatchedAt = ctx.ticks;
        }
        if (ctx.ticks - sRecolorMatchedAt < step.value("frames", 0)) {
            return false;
        }
        TwiliLog.info("[autotest] dummy recolor: {}", probeText(probe));
        return true;
    }
    if (sRecolorMatchedAt >= 0) {
        ctx.fail(fmt::format("expectDummyRecolor: {} ticks after it matched, {} ({})",
            ctx.ticks - sRecolorMatchedAt, why, probeText(probe)));
        return false;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format(
            "expectDummyRecolor: {} ({})", why, dummy != nullptr ? probeText(probe) : "no probe"));
    }
    return false;
}

struct ProcessMemory {
    uint64_t gpu = 0;
    uint64_t privateBytes = 0;
    bool gpuValid = false;
    std::string adapters;  // local/non-local MiB of each adapter this process uses
};

ProcessMemory queryProcessMemory() {
    ProcessMemory m;
#if _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc)))
    {
        m.privateBytes = pmc.PrivateUsage;
    }
    static HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        dxgi != nullptr ?
            reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) :
            nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create == nullptr ||
        FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        return m;
    }
    IDXGIAdapter1* adapter = nullptr;
    // One GPU can be listed more than once, even under different LUIDs.
    std::vector<DXGI_ADAPTER_DESC1> seen;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIAdapter3* adapter3 = nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        const bool known = FAILED(adapter->GetDesc1(&desc)) ||
                           std::any_of(seen.begin(), seen.end(), [&](const DXGI_ADAPTER_DESC1& d) {
                               return d.VendorId == desc.VendorId &&
                                      d.DeviceId == desc.DeviceId &&
                                      d.SubSysId == desc.SubSysId &&
                                      d.Revision == desc.Revision;
                           });
        seen.push_back(desc);
        if (!known && SUCCEEDED(adapter->QueryInterface(
                          __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))))
        {
            uint64_t usage[2] = {};
            for (int g = 0; g < 2; g++) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,
                        g == 0 ? DXGI_MEMORY_SEGMENT_GROUP_LOCAL :
                                 DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                        &info)))
                {
                    usage[g] = info.CurrentUsage;
                    m.gpu += info.CurrentUsage;
                    m.gpuValid = true;
                }
            }
            if (usage[0] + usage[1] != 0) {
                std::string name;
                for (const WCHAR* c = desc.Description; *c != 0 && name.size() < 40; c++) {
                    name += *c < 128 ? static_cast<char>(*c) : '?';
                }
                m.adapters += fmt::format(" [{} {:.1f}/{:.1f}]", name,
                    usage[0] / (1024.0 * 1024.0), usage[1] / (1024.0 * 1024.0));
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    factory->Release();
#endif
    return m;
}

uint32_t sMarkApplies = 0;
uint32_t sMarkDraws = 0;
ProcessMemory sMarkMemory;

double mib(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

int sCycleIndex = 0;

color::Rgb8 cycleColor(int i, double s, double v) {
    color::Rgb8 c = color::hsvToRgb({std::fmod(i * 137.50776, 360.0), s, v});
    if (c == color::Rgb8{255, 255, 255}) {
        c.b = 254;  // exact white means the original colours
    }
    return c;
}

std::optional<bool> recolorSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "recolorSelfTest") {
        const std::string failure = recolorSelfTest();
        if (!failure.empty()) {
            ctx.fail("recolorSelfTest: " + failure);
            return false;
        }
        TwiliLog.info("[autotest] recolour maths OK");
        return true;
    }

    if (op == "expectDummyRecolor") {
        return expectDummyRecolor(ctx);
    }

    if (op == "markRecolor" || op == "expectRecolorDelta") {
        fopAc_ac_c* dummy = peerDummy(step.value("name", std::string{}));
        RecolorProbe probe;
        if (dummy == nullptr || !GetDummyPlayerRecolorProbe(dummy, probe)) {
            ctx.fail(op + ": no peer dummy in our layer");
            return false;
        }
        const ProcessMemory mem = queryProcessMemory();
        if (op == "markRecolor") {
            sMarkApplies = probe.applyCount;
            sMarkDraws = probe.draws;
            sMarkMemory = mem;
            TwiliLog.info("[autotest] recolor mark: {} applies, gpu {:.1f} MiB{}{}, private "
                          "{:.1f} MiB",
                probe.applyCount, mib(mem.gpu), mem.gpuValid ? "" : " (unavailable)",
                mem.adapters, mib(mem.privateBytes));
            return true;
        }
        const uint32_t applies = probe.applyCount - sMarkApplies;
        const uint32_t draws = probe.draws - sMarkDraws;
        const double gpuGrowth = mib(static_cast<int64_t>(mem.gpu - sMarkMemory.gpu));
        const double privateGrowth =
            mib(static_cast<int64_t>(mem.privateBytes - sMarkMemory.privateBytes));
        TwiliLog.info("[autotest] recolor delta: {} applies (last {:.2f} ms), {} draws, gpu "
                      "{:.1f} -> {:.1f} MiB ({:+.1f}){}, private {:.1f} -> {:.1f} MiB ({:+.1f})",
            applies, probe.lastApplyMs, draws, mib(sMarkMemory.gpu), mib(mem.gpu), gpuGrowth,
            mem.adapters, mib(sMarkMemory.privateBytes), mib(mem.privateBytes), privateGrowth);
        std::string why;
        if (applies < step.value("minApplies", 0u) || applies > step.value("maxApplies", ~0u)) {
            why = fmt::format("{} applies", applies);
        } else if (draws < step.value("minDraws", 0u)) {
            why = fmt::format("drawn {} times", draws);
        } else if (step.contains("maxGpuGrowthMB") && !mem.gpuValid) {
            why = "no GPU memory figures";
        } else if (step.contains("maxGpuGrowthMB") &&
                   gpuGrowth > step["maxGpuGrowthMB"].get<double>())
        {
            why = fmt::format("GPU memory grew {:.1f} MiB", gpuGrowth);
        } else if (step.contains("maxPrivateGrowthMB") &&
                   privateGrowth > step["maxPrivateGrowthMB"].get<double>())
        {
            why = fmt::format("private memory grew {:.1f} MiB", privateGrowth);
        }
        if (!why.empty()) {
            ctx.fail("expectRecolorDelta: " + why);
            return false;
        }
        return true;
    }

    if (op == "colorCycle") {
        const int count = step.value("count", 200);
        const int every = std::max(1, step.value("everyTicks", 8));
        const double s = step.value("s", 0.8), v = step.value("v", 0.85);
        if (!ctx.begun) {
            sCycleIndex = 0;
        }
        // One change per call at most: one update can run several ticks.
        if (sCycleIndex < count && ctx.ticks >= sCycleIndex * every) {
            config::setString(config::Var::Color, color::formatConfig(cycleColor(sCycleIndex, s, v)));
            sCycleIndex++;
        }
        if (sCycleIndex < count) {
            return false;
        }
        TwiliLog.info("[autotest] colour cycle done: {} colours, last {}", count,
            color::formatHex(cycleColor(count - 1, s, v)));
        return true;
    }

    if (op == "setRupees") {
        const int value = std::clamp(step.value("value", 0), 0, 9999);
        if (!ctx.begun) {
            dComIfGs_setRupee(static_cast<u16>(value));
        }
        daAlink_c* link = daAlink_getAlinkActorClass();
        // Only game.armorRupeeDrain NORMAL drains at 0 rupees (checkMagicArmorHeavy says so).
        const bool armor = link != nullptr && link->checkMagicArmorWearAbility() &&
                           (value != 0 || link->checkMagicArmorHeavy());
        if (!armor || (link->field_0x2fd7 == 0) == (value == 0)) {
            TwiliLog.info("[autotest] rupees set to {}{}", value,
                armor ? (value == 0 ? ", Magic Armor drained" : ", Magic Armor powered") : "");
            return true;
        }
        if (ctx.seconds > ctx.timeout(5.0)) {
            ctx.fail(fmt::format("setRupees: the Magic Armor BRK is still {}", link->field_0x2fd7));
        }
        return false;
    }

    if (op == "recolorCamera") {
        fopAc_ac_c* actor = step.value("target", std::string("dummy")) == "self" ?
                                static_cast<fopAc_ac_c*>(daAlink_getAlinkActorClass()) :
                                peerDummy(step.value("name", std::string{}));
        camera_process_class* camera = dComIfGp_getCamera(dComIfGp_getPlayerCameraID(0));
        if (actor == nullptr || camera == nullptr) {
            ctx.fail("recolorCamera: no camera or no one to look at");
            return false;
        }
        const s16 yaw = actor->shape_angle.y;
        const cXyz front(cM_ssin(yaw), 0.0f, cM_scos(yaw));
        const cXyz right(front.z, 0.0f, -front.x);
        const cXyz center = actor->current.pos + cXyz(0.0f, step.value("height", 75.0f), 0.0f);
        const cXyz eye = center + front * step.value("dist", 150.0f) +
                         right * step.value("side", 0.0f) +
                         cXyz(0.0f, step.value("up", 25.0f), 0.0f);
        camera->mCamera.Reset(center, eye);
        return true;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&recolorSteps);

}  // namespace
}  // namespace twili::autotest
