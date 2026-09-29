// Steps for remote players on the maps; reference in the runner README.

#include "autotest/AutoTestSteps.hpp"

#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Session.hpp"
#include "ui/MapCursors.hpp"

#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_map.h"
#include "d/d_map_path_dmap.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace twili::autotest {
namespace {

using nlohmann::json;
namespace mc = ui::map_cursor;

constexpr f32 kTip = 400.0f / 640.0f;
constexpr f32 kMaxFacingErrorDeg = 3.0f;

// First tick a hold condition was met, or -1.
int sHoldSince = -1;

struct Rgb {
    u8 r = 0, g = 0, b = 0;
};

bool parseHex(const std::string& s, Rgb& out) {
    if (s.size() != 7 || s[0] != '#') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
    if (end != s.c_str() + 7) return false;
    out = {static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
    return true;
}

std::string hex(const u8* c) {
    return fmt::format("#{:02X}{:02X}{:02X}", c[0], c[1], c[2]);
}

// A peer by name, online or not; nullptr when no client has that name.
const Client* clientNamed(const std::string& name) {
    for (const auto& [id, c] : Session::instance().clients()) {
        if (!c.self && c.name == name) return &c;
    }
    return nullptr;
}

// "#<id>" or a peer name; 0 when unknown.
uint32_t resolveTarget(const std::string& target) {
    if (target.size() > 1 && target[0] == '#') {
        return static_cast<uint32_t>(std::strtoul(target.c_str() + 1, nullptr, 10));
    }
    const Client* c = clientNamed(target);
    return c != nullptr ? c->clientId : 0;
}

mc::Surface surfaceOf(const json& step) {
    const std::string s = step.value("surface", std::string("minimap"));
    return s == "dmap" ? mc::Surface::PauseDmap
         : s == "fmap" ? mc::Surface::PauseFmap
                       : mc::Surface::Minimap;
}

bool fresh(const mc::Frame& f, const json& step) {
    return f.valid && g_Counter.mCounter0 - f.tick <= step.value("maxAgeTicks", 3u);
}

const mc::Drawn* findDrawn(const mc::Frame& f, uint32_t id) {
    for (const mc::Drawn& d : f.cursors) {
        if (d.clientId == id) return &d;
    }
    return nullptr;
}

f32 angleBetweenDeg(mc::Vec2 a, mc::Vec2 b) {
    const f32 la = std::sqrt(a.x * a.x + a.y * a.y);
    const f32 lb = std::sqrt(b.x * b.x + b.y * b.y);
    if (la < 1e-6f || lb < 1e-6f) return 180.0f;
    const f32 c = std::clamp((a.x * b.x + a.y * b.y) / (la * lb), -1.0f, 1.0f);
    return std::acos(c) * 180.0f / 3.14159265f;
}

std::string describe(const mc::Frame& f) {
    std::string s = fmt::format("frame valid={} age={} gate={} alpha={} markers=[", f.valid,
                                g_Counter.mCounter0 - f.tick, mc::gateName(f.gate), f.mapAlpha);
    for (const mc::Drawn& d : f.cursors) {
        s += fmt::format(" #{} {} a={} at ({:.1f},{:.1f}) dir ({:.2f},{:.2f}){}{}", d.clientId,
                         hex(d.fill), d.fill[3], d.anchor.x, d.anchor.y, d.dir.x, d.dir.y,
                         d.pinned ? " pinned" : "",
                         d.floorDelta != 0 ? fmt::format(" floor{:+d}", d.floorDelta) : "");
    }
    s += " ] skipped=[";
    for (const mc::Skipped& k : f.skipped) {
        s += fmt::format(" #{} {}", k.clientId, mc::skipName(k.reason));
    }
    return s + " ]";
}

// A condition that must hold for `holdTicks` consecutive checks' worth of game ticks.
bool held(bool ok, const StepContext& ctx, int holdTicks) {
    if (!ctx.begun) sHoldSince = -1;
    if (!ok) {
        sHoldSince = -1;
        return false;
    }
    if (sHoldSince < 0) sHoldSince = ctx.ticks;
    return ctx.ticks - sHoldSince >= holdTicks;
}

// The minimap anchor of (x, z) as the offscreen pass computes it, then NDC onto the rect.
mc::Vec2 referenceAnchor(const mc::MinimapXform& t, f32 x, f32 z) {
    Mtx view;
    Vec eye = {t.centerX, t.centerZ, -5000.0f};
    Vec at = {t.centerX, t.centerZ, 5000.0f};
    Vec up = {0.0f, -1.0f, 0.0f};
    mDoMtx_lookAt(view, &eye, &at, &up, 0);
    Mtx44 proj;
    const f32 right = (t.mirror ? -0.5f : 0.5f) * t.worldW;
    const f32 top = 0.5f * t.worldH;
    C_MTXOrtho(proj, top, -top, -right, right, 0.0f, 10000.0f);
    Vec p = {x, z, 0.0f};
    Vec v;
    MTXMultVec(view, &p, &v);
    const f32 ndcX = proj[0][0] * v.x + proj[0][1] * v.y + proj[0][3];
    const f32 ndcY = proj[1][0] * v.x + proj[1][1] * v.y + proj[1][3];
    return {t.rectX + (ndcX + 1.0f) * 0.5f * t.rectW, t.rectY + (1.0f - ndcY) * 0.5f * t.rectH};
}

mc::Vec2 referenceFacing(const mc::MinimapXform& t, const mc::MapPose& p) {
    // 64 texels ahead: far enough that f32 rounding cannot tilt it.
    const f32 step = 64.0f * t.worldW / t.texW;
    const mc::Vec2 a = referenceAnchor(t, p.x, p.z);
    const mc::Vec2 b = referenceAnchor(t, p.x + cM_ssin(p.angle) * step,
                                       p.z + cM_scos(p.angle) * step);
    return {b.x - a.x, b.y - a.y};
}

std::optional<bool> expectMapCursor(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(surfaceOf(step));
    std::vector<std::string> targets;
    if (step.contains("targets")) {
        targets = step["targets"].get<std::vector<std::string>>();
    } else {
        targets.push_back(step.value("target", std::string{}));
    }
    std::vector<std::string> colors;
    if (step.contains("colors")) {
        colors = step["colors"].get<std::vector<std::string>>();
    } else if (step.contains("color")) {
        colors.assign(targets.size(), step.value("color", std::string{}));
    }
    if (!colors.empty() && colors.size() != targets.size()) {
        ctx.fail("expectMapCursor: colors and targets differ in length");
        return false;
    }

    std::string why;
    bool ok = fresh(f, step) && f.gate == mc::Gate::Drawn;
    if (!ok) {
        why = "no fresh frame that ran the filters";
    }
    if (ok && step.contains("count") && f.cursors.size() != step.value("count", size_t(0))) {
        ok = false;
        why = fmt::format("{} markers, want {}", f.cursors.size(), step.value("count", size_t(0)));
    }
    for (size_t i = 0; ok && i < targets.size(); i++) {
        const uint32_t id = resolveTarget(targets[i]);
        const mc::Drawn* d = id != 0 ? findDrawn(f, id) : nullptr;
        if (d == nullptr) {
            ok = false;
            why = fmt::format("no marker for {} (client {})", targets[i], id);
            break;
        }
        if (!colors.empty()) {
            Rgb want;
            if (colors[i] == "peer") {
                const Client* c = clientNamed(targets[i]);
                if (c == nullptr) {
                    ctx.fail("expectMapCursor: color \"peer\" needs a peer name");
                    return false;
                }
                want = {c->colorR, c->colorG, c->colorB};
            } else if (!parseHex(colors[i], want)) {
                ctx.fail("expectMapCursor: bad colour '" + colors[i] + "'");
                return false;
            }
            if (d->fill[0] != want.r || d->fill[1] != want.g || d->fill[2] != want.b) {
                ok = false;
                why = fmt::format("{} is drawn {}, want {}", targets[i], hex(d->fill), colors[i]);
                break;
            }
        }
        // On our floor the fill is fully the map's alpha: nothing but the map fades it.
        if (d->floorDelta == 0 && d->fill[3] != f.mapAlpha) {
            ok = false;
            why = fmt::format("{} alpha {} but the map's is {}", targets[i], d->fill[3], f.mapAlpha);
            break;
        }
        if (step.contains("pinned") && d->pinned != step.value("pinned", false)) {
            ok = false;
            why = fmt::format("{} pinned={}", targets[i], d->pinned);
            break;
        }
        if (step.contains("floorDelta") && d->floorDelta != step.value("floorDelta", 0)) {
            ok = false;
            why = fmt::format("{} floorDelta={}", targets[i], d->floorDelta);
            break;
        }
        if (step.contains("facing")) {
            const auto v = step["facing"].get<std::vector<f32>>();
            const f32 err = v.size() == 2 ? angleBetweenDeg(d->dir, {v[0], v[1]}) : 180.0f;
            if (err > kMaxFacingErrorDeg) {
                ok = false;
                why = fmt::format("{} faces ({:.3f}, {:.3f}), {:.1f} degrees off", targets[i],
                                  d->dir.x, d->dir.y, err);
                break;
            }
        }
    }

    if (held(ok, ctx, step.value("holdTicks", 0))) {
        TwiliLog.info("[autotest] map cursors ok: {}", describe(f));
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("expectMapCursor: {}; {}", why.empty() ? "did not hold" : why,
                             describe(f)));
    }
    return false;
}

std::optional<bool> expectNoMapCursor(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(surfaceOf(step));
    const std::string target = step.value("target", std::string{});
    const std::string reason = step.value("reason", std::string{});

    static const char* const kGates[] = {"alphaZero", "notConnected", "locationsOff", "cutscene",
                                         "noStage", "notInField", "otherRegion"};
    static const char* const kSkips[] = {"noSave", "noUpdate", "otherStage", "otherLayer",
                                         "otherFloor", "badPose"};
    const bool gateReason = std::find(std::begin(kGates), std::end(kGates), reason) != std::end(kGates);
    const bool skipReason = std::find(std::begin(kSkips), std::end(kSkips), reason) != std::end(kSkips);
    if (!gateReason && !skipReason && reason != "absent") {
        ctx.fail("expectNoMapCursor: unknown reason '" + reason + "'");
        return false;
    }

    std::string why;
    bool ok = fresh(f, step);
    if (!ok) {
        why = "no fresh frame";
    } else if (gateReason) {
        ok = std::strcmp(mc::gateName(f.gate), reason.c_str()) == 0 && f.cursors.empty();
        if (!ok) why = fmt::format("gate is {}", mc::gateName(f.gate));
    } else if (f.gate != mc::Gate::Drawn) {
        ok = false;
        why = fmt::format("the frame did not run the filters (gate {})", mc::gateName(f.gate));
    } else if (reason == "absent") {
        const bool known = clientNamed(target) != nullptr;
        // Every real marker belongs to a client that still exists.
        bool orphan = false;
        for (const mc::Drawn& d : f.cursors) {
            orphan = orphan || (!d.injected &&
                                Session::instance().clients().count(d.clientId) == 0);
        }
        ok = !known && !orphan;
        if (!ok) why = known ? target + " is still a client" : "a marker has no client";
    } else {
        const uint32_t id = resolveTarget(target);
        const auto skip = std::find_if(f.skipped.begin(), f.skipped.end(),
                                       [&](const mc::Skipped& s) { return s.clientId == id; });
        ok = id != 0 && findDrawn(f, id) == nullptr && skip != f.skipped.end() &&
             std::strcmp(mc::skipName(skip->reason), reason.c_str()) == 0;
        if (!ok) {
            why = id == 0                        ? "no client named " + target
                  : findDrawn(f, id) != nullptr ? target + " is drawn"
                  : skip == f.skipped.end()     ? target + " was not skipped"
                                                : fmt::format("{} skipped for {}", target,
                                                              mc::skipName(skip->reason));
        }
    }

    if (held(ok, ctx, step.value("holdTicks", 30))) {
        TwiliLog.info("[autotest] no map cursor for {} ({}): {}", target, reason, describe(f));
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail(fmt::format("expectNoMapCursor {} ({}): {}; {}", target, reason, why,
                             describe(f)));
    }
    return false;
}

std::optional<bool> checkMapCursorTransform(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(mc::Surface::Minimap);
    if (!fresh(f, step)) {
        if (ctx.seconds > ctx.timeout(10.0)) ctx.fail("checkMapCursorTransform: no fresh frame");
        return false;
    }
    const f32 tol = step.value("tolPx", 0.75f);
    const mc::MinimapXform& t = f.xform;
    const auto near = [&](mc::Vec2 a, mc::Vec2 b) {
        return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol;
    };

    const mc::Vec2 local = referenceAnchor(t, f.local.x, f.local.z);
    if (!near(local, f.localAnchor)) {
        ctx.fail(fmt::format("checkMapCursorTransform: our anchor ({:.2f}, {:.2f}), reference "
                             "({:.2f}, {:.2f})",
                             f.localAnchor.x, f.localAnchor.y, local.x, local.y));
        return false;
    }
    const f32 localErr = angleBetweenDeg(f.localDir, referenceFacing(t, f.local));
    if (localErr > kMaxFacingErrorDeg) {
        ctx.fail(fmt::format("checkMapCursorTransform: our facing is {:.1f} degrees off", localErr));
        return false;
    }

    const std::string target = step.value("target", std::string{});
    const uint32_t only = target.empty() ? 0 : resolveTarget(target);
    int checked = 0;
    for (const mc::Drawn& d : f.cursors) {
        if (only != 0 && d.clientId != only) continue;
        if (d.pinned) {
            if (only != 0) {
                ctx.fail("checkMapCursorTransform: " + target + " is pinned to the edge");
                return false;
            }
            continue;
        }
        const mc::Vec2 ref = referenceAnchor(t, d.pose.x, d.pose.z);
        const f32 err = angleBetweenDeg(d.dir, referenceFacing(t, d.pose));
        const f32 tipLen = std::sqrt((d.tip.x - d.anchor.x) * (d.tip.x - d.anchor.x) +
                                     (d.tip.y - d.anchor.y) * (d.tip.y - d.anchor.y));
        if (!near(ref, d.anchor) || err > kMaxFacingErrorDeg ||
            std::fabs(tipLen - kTip * d.height) > 0.01f)
        {
            ctx.fail(fmt::format("checkMapCursorTransform: #{} at ({:.2f}, {:.2f}) reference "
                                 "({:.2f}, {:.2f}), facing {:.1f} degrees off, tip {:.2f} of {:.2f}",
                                 d.clientId, d.anchor.x, d.anchor.y, ref.x, ref.y, err, tipLen,
                                 kTip * d.height));
            return false;
        }
        checked++;
    }
    if (only != 0 && checked == 0) {
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail("checkMapCursorTransform: no marker for " + target + "; " + describe(f));
        }
        return false;
    }
    TwiliLog.info("[autotest] map transform ok: {} marker(s), world {:.0f}x{:.0f} cm on {}x{} "
                 "texels, rect ({:.1f}, {:.1f}) {:.0f}x{:.0f}, mirror={}",
                 checked, t.worldW, t.worldH, t.texW, t.texH, t.rectX, t.rectY, t.rectW, t.rectH,
                 t.mirror);
    return true;
}

// The field map places remote players as it placed its Link icon: ours lands under it.
std::optional<bool> checkFmapLinkIcon(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(mc::Surface::PauseFmap);
    const f32 tol = step.value("tolPx", 1.0f);
    if (!fresh(f, step) || f.gate != mc::Gate::Drawn || !f.linkIconValid) {
        if (ctx.seconds > ctx.timeout(10.0)) {
            ctx.fail(fmt::format("checkFmapLinkIcon: no Link icon on a fresh field map frame; {}",
                                 describe(f)));
        }
        return false;
    }
    const f32 dx = f.localAnchor.x - f.linkIcon.x;
    const f32 dy = f.localAnchor.y - f.linkIcon.y;
    if (std::fabs(dx) > tol || std::fabs(dy) > tol) {
        ctx.fail(fmt::format("checkFmapLinkIcon: our position placed at ({:.2f}, {:.2f}), the Link "
                             "icon at ({:.2f}, {:.2f})",
                             f.localAnchor.x, f.localAnchor.y, f.linkIcon.x, f.linkIcon.y));
        return false;
    }
    TwiliLog.info("[autotest] field map: our position ({:.2f}, {:.2f}) is under the Link icon "
                  "({:.2f}, {:.2f})",
                  f.localAnchor.x, f.localAnchor.y, f.linkIcon.x, f.linkIcon.y);
    return true;
}

// A remote Epona's horseshoe on the minimap, in its owner's colour.
std::optional<bool> expectMapHorse(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(mc::Surface::Minimap);
    const std::string target = step.value("target", std::string{});
    const uint32_t id = resolveTarget(target);
    std::string why = "no fresh frame that ran the filters";
    bool ok = false;
    if (fresh(f, step) && f.gate == mc::Gate::Drawn) {
        why = fmt::format("no horse icon for {}", target);
        for (const mc::HorseIcon& h : f.horses) {
            if (h.clientId != id) continue;
            ok = true;
            why.clear();
            if (step.contains("ridden") && h.ridden != step.value("ridden", false)) {
                ok = false;
                why = fmt::format("{}'s horse ridden={}", target, h.ridden);
            }
            if (ok && step.contains("ownerAway") &&
                h.ownerAway != step.value("ownerAway", false)) {
                ok = false;
                why = fmt::format("{}'s horse ownerAway={}", target, h.ownerAway);
            }
            if (ok && step.contains("color")) {
                Rgb want;
                const std::string col = step.value("color", std::string{});
                if (col == "peer") {
                    const Client* c = clientNamed(target);
                    if (c != nullptr) want = {c->colorR, c->colorG, c->colorB};
                } else if (!parseHex(col, want)) {
                    ctx.fail("expectMapHorse: bad colour '" + col + "'");
                    return false;
                }
                if (h.fill[0] != want.r || h.fill[1] != want.g || h.fill[2] != want.b) {
                    ok = false;
                    why = fmt::format("{}'s horse is drawn {}", target, hex(h.fill));
                }
            }
            if (ok) {
                why = fmt::format("{} at ({:.1f}, {:.1f}) size {:.1f} ridden={} ownerAway={}",
                                  hex(h.fill), h.anchor.x, h.anchor.y, h.size, h.ridden,
                                  h.ownerAway);
            }
            break;
        }
    }
    if (held(ok, ctx, step.value("holdTicks", 0))) {
        TwiliLog.info("[autotest] map horse of {}: {}", target, why);
        return true;
    }
    if (ctx.seconds > ctx.timeout(20.0)) {
        ctx.fail("expectMapHorse: " + why + "; " + describe(f));
    }
    return false;
}

std::optional<bool> injectClients(StepContext& ctx) {
    const json& step = ctx.step;
    const mc::Frame& f = mc::lastFrame(mc::Surface::Minimap);
    f32 scale = 1.0f;
    if (step.value("unit", std::string("cm")) == "texel") {
        if (!fresh(f, step) || f.xform.texW == 0) {
            if (ctx.seconds > ctx.timeout(10.0)) {
                ctx.fail("injectMapCursorClients: texel offsets need a fresh minimap frame");
            }
            return false;
        }
        scale = f.xform.worldW / f.xform.texW;
    }
    if (daPy_getPlayerActorClass() == nullptr) {
        ctx.fail("injectMapCursorClients: no player");
        return false;
    }
    const Vec base = dMapInfo_n::getMapPlayerPos();
    std::vector<mc::TestClient> out;
    for (const json& c : step.value("clients", json::array())) {
        mc::TestClient t;
        Rgb rgb;
        if (!parseHex(c.value("color", std::string{}), rgb)) {
            ctx.fail("injectMapCursorClients: bad colour " + c.dump());
            return false;
        }
        t.clientId = c.value("id", 0u);
        t.r = rgb.r;
        t.g = rgb.g;
        t.b = rgb.b;
        t.x = base.x + c.value("dx", 0.0f) * scale;
        t.y = base.y + c.value("dy", 0.0f);
        t.z = base.z + c.value("dz", 0.0f) * scale;
        t.angle = static_cast<s16>(c.value("angle", 0));
        t.roomNo = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
        out.push_back(t);
    }
    TwiliLog.info("[autotest] injected {} map cursor client(s) around ({:.0f}, {:.0f}), {:.1f} cm "
                 "per unit",
                 out.size(), base.x, base.z, scale);
    mc::setTestClients(std::move(out));
    return true;
}

std::optional<bool> expectPaletteClean(StepContext& ctx) {
    const json& step = ctx.step;
    dMap_prm_res_s* res = dMap_HIO_prm_res_dst_s::m_res;
    const auto* src = static_cast<const dMap_prm_res_s*>(dComIfG_getObjectRes("Always", 0x45));
    const mc::Frame& f = mc::lastFrame(mc::Surface::Minimap);
    if (!fresh(f, step) || res == nullptr || src == nullptr) {
        if (ctx.seconds > ctx.timeout(10.0)) ctx.fail("expectMapPaletteClean: no minimap");
        return false;
    }
    // Only a dungeon map keeps its source palette (fields rescale groups 1-44).
    const auto groups = step.value("groups", std::vector<int>{42, 43, 44, 47, 48, 49});
    for (int g : groups) {
        if (g < 0 || g >= 51) {
            ctx.fail(fmt::format("expectMapPaletteClean: bad group {}", g));
            return false;
        }
        if (std::memcmp(&res->palette_data[g], &src->palette_data[g], sizeof(res->palette_data[g])) != 0) {
            ctx.fail(fmt::format("expectMapPaletteClean: group {} is {:04X}, source {:04X}", g,
                                 static_cast<u16>(res->palette_data[g].field_0x0.color),
                                 static_cast<u16>(src->palette_data[g].field_0x0.color)));
            return false;
        }
    }
    if (held(true, ctx, step.value("holdTicks", 60))) {
        TwiliLog.info("[autotest] map palette groups untouched with {} marker(s) drawn",
                     f.cursors.size());
        return true;
    }
    return false;
}

void dump(const mc::Frame& f) {
    TwiliLog.info("[autotest] map cursors: {}", describe(f));
    // Fractions of the 2D viewport, which the window shows whole.
    const f32 minX = mDoGph_gInf_c::getMinXF();
    const f32 minY = mDoGph_gInf_c::getMinYF();
    const f32 w = mDoGph_gInf_c::getWidthF();
    const f32 h = mDoGph_gInf_c::getHeightF();
    const auto probe = [&](const char* what, mc::Vec2 anchor, mc::Vec2 dir, f32 height,
                           const std::string& rgb) {
        // Centroid of drawCursor's triangle: (400 - 240 - 240) / 3 / 640 ahead of the anchor.
        const f32 k = (400.0f - 480.0f) / 3.0f / 640.0f * height;
        const f32 cx = anchor.x + dir.x * k;
        const f32 cy = anchor.y + dir.y * k;
        TwiliLog.info("[autotest] MAPCURSOR_PROBE {} fx={:.4f} fy={:.4f} rgb={}", what,
                     (cx - minX) / w, (cy - minY) / h, rgb);
    };
    // Our arrow is in the map texture, in palette group 0x1E.
    std::string localRgb = "?";
    if (dMap_HIO_prm_res_dst_s::m_res != nullptr) {
        GXColor c;
        dMpath_ColorCnv_n::convertRGB5A3_To_GXColor(
            c, dMap_HIO_prm_res_dst_s::m_res->palette_data[0x1E].field_0x0);
        const u8 rgb[3] = {c.r, c.g, c.b};
        localRgb = hex(rgb);
    }
    if (f.surface == mc::Surface::Minimap) {
        probe(f.localRedrawn ? "local(redrawn)" : "local", f.localAnchor, f.localDir,
              f.localHeight, localRgb);
    }
    for (const mc::Drawn& d : f.cursors) {
        probe(fmt::format("#{}", d.clientId).c_str(), d.anchor, d.dir, d.height, hex(d.fill));
    }
}

std::optional<bool> mapCursorSteps(const std::string& op, StepContext& ctx) {
    const json& step = ctx.step;

    if (op == "mapCursorSelfTest") {
        std::string failure;
        if (!mc::selfTest(failure)) {
            ctx.fail("mapCursorSelfTest: " + failure);
            return false;
        }
        TwiliLog.info("[autotest] map cursor self-test passed");
        return true;
    }

    if (op == "showMinimap") {
        if (config::hostBool("game.minimalHUD", false) ||
            config::hostBool("game.recordingMode", false) ||
            config::hostBool("game.debugFlyCam", false))
        {
            ctx.fail("showMinimap: minimalHUD, recordingMode or debugFlyCam hides the HUD");
            return false;
        }
        const mc::Frame& f = mc::lastFrame(mc::Surface::Minimap);
        if (fresh(f, step) && f.rectOnScreen && f.mapAlpha >= step.value("minAlpha", 255)) {
            TwiliLog.info("[autotest] minimap shown: {}", describe(f));
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("showMinimap: {} onScreen={} (no minimap in this stage?)",
                                 describe(f), f.rectOnScreen));
        }
        return false;
    }

    if (op == "injectMapCursorClients") return injectClients(ctx);

    if (op == "clearMapCursorClients") {
        mc::setTestClients({});
        return true;
    }

    if (op == "expectMapCursor") return expectMapCursor(ctx);
    if (op == "checkFmapLinkIcon") return checkFmapLinkIcon(ctx);
    if (op == "expectMapHorse") return expectMapHorse(ctx);
    if (op == "expectNoMapCursor") return expectNoMapCursor(ctx);
    if (op == "checkMapCursorTransform") return checkMapCursorTransform(ctx);
    if (op == "expectMapPaletteClean") return expectPaletteClean(ctx);

    if (op == "dumpMapCursors") {
        dump(mc::lastFrame(surfaceOf(step)));
        return true;
    }

    if (op == "openPauseMap") {
        const bool field = surfaceOf(step) == mc::Surface::PauseFmap;
        if (!ctx.begun) {
            if (field) {
                // The PC map key's path (dMeterMap_c::ctrlShowMap)
                dMeter2Info_setMapStatus(2);
                dMeter2Info_setMapKeyDirection(0x400);
            } else {
                dMeter2Info_setPauseStatus(4);
            }
        }
        const mc::Frame& f = mc::lastFrame(field ? mc::Surface::PauseFmap : mc::Surface::PauseDmap);
        if (fresh(f, step) && f.mapAlpha > 0) {
            TwiliLog.info("[autotest] pause map open: {}", describe(f));
            return true;
        }
        if (ctx.seconds > ctx.timeout(20.0)) {
            ctx.fail(fmt::format("openPauseMap: mapStatus={} pauseStatus={}; {}",
                                 dMeter2Info_getMapStatus(), dMeter2Info_getPauseStatus(),
                                 describe(f)));
        }
        return false;
    }

    return std::nullopt;
}

const bool sRegistered = registerSteps(&mapCursorSteps);

}  // namespace
}  // namespace twili::autotest
