#include "ui/MapCursors.hpp"

#include "core/Config.hpp"
#include "core/Session.hpp"
#include "core/Visibility.hpp"

#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "SSystem/SComponent/c_counter.h"
#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "d/d_map.h"
#include "d/d_map_path_dmap.h"
#include "d/d_menu_map_common.h"
#include "d/d_stage.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace twili::ui::map_cursor {
namespace {

// drawCursor's triangle: tip at 400, base at -240, half width 200, in 1/640ths of the size.
constexpr f32 kTip = 400.0f / 640.0f;
constexpr f32 kBase = 240.0f / 640.0f;
constexpr f32 kHalfWidth = 200.0f / 640.0f;
// Remote arrows a little smaller than ours, so the local one stays the dominant marker.
constexpr f32 kRemoteScale = 0.85f;
constexpr f32 kMinArrow = 7.0f;
constexpr f32 kOutline = 1.25f;
constexpr f32 kPinScale = 0.75f;
constexpr f32 kOtherFloorAlpha = 0.55f;
// Pause dungeon map: the Link icon is 40 units at scale 1.
constexpr f32 kPauseArrow = 22.0f;
// Palette group of the local cursor.
constexpr int kLocalCursorGroup = 0x1E;

Frame sFrames[size_t(Surface::Count)];
std::vector<TestClient> sTestClients;

struct Source {
    uint32_t clientId = 0;
    u8 r = 255, g = 255, b = 255;
    MapPose pose;
    bool injected = false;
};

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(Vec2 a, f32 s) { return {a.x * s, a.y * s}; }
f32 length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
f32 cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

Vec2 normalized(Vec2 a, Vec2 fallback = {0.0f, -1.0f}) {
    const f32 len = length(a);
    return len > 1e-6f ? a * (1.0f / len) : fallback;
}

u8 scaled(u8 value, f32 factor) {
    return static_cast<u8>(std::clamp(std::lround(value * factor), 0l, 255l));
}

bool mirrorMode() {
    return config::hostBool("game.enableMirrorMode", false);
}

// The local cursor's correction (dMapInfo_n::getMapPlayerPos) with the remote's own room.
bool remoteMapPose(const Client& c, MapPose& out) {
    BE(Vec) pos;
    pos.x = c.posX;
    pos.y = c.posY;
    pos.z = c.posZ;
    s16 angle = c.shapeAngleY;
    if (c.roomNo >= 0 && c.roomNo < 64) {
        dMapInfo_n::correctionOriginPos(c.roomNo, &pos);
        if (const dStage_FileList2_dt_c* fl = dStage_roomControl_c::getFileList2(c.roomNo)) {
            angle += fl->field_0x1c;
        }
    }
    const Vec v = pos;
    out = {v.x, v.y, v.z, angle, c.roomNo};
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The room-wide gates and per-client filters every surface shares. `gate` stays Drawn when the
// filters ran.
void gatherSources(std::vector<Source>& out, std::vector<Skipped>& skipped, Gate& gate) {
    out.clear();
    skipped.clear();
    gate = Gate::Drawn;
    if (!Session::active() || !Session::instance().isConnected()) {
        gate = Gate::NotConnected;
        return;
    }
    const Session& session = Session::instance();
    if (!session.roomState().showLocationsMode) {
        gate = Gate::LocationsOff;
        return;
    }
    // A remote's own cutscene keeps its marker: the location is still useful.
    if (hideRemotePlayersForCutscene()) {
        gate = Gate::CutsceneHidden;
        return;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr) {
        gate = Gate::NoStage;
        return;
    }
    const int8_t layer = static_cast<int8_t>(dComIfG_play_c::getLayerNo(0));
    for (const auto& [id, c] : session.clients()) {
        if (c.self) {
            continue;
        }
        Skip why;
        MapPose pose;
        if (!c.online || !c.isSaveLoaded) {
            why = Skip::NoSave;
        } else if (std::strncmp(c.stageName, stage, sizeof(c.stageName)) != 0) {
            why = Skip::OtherStage;
        } else if (c.layerNo != layer) {
            why = Skip::OtherLayer;
        } else if (!c.hasPlayerUpdate) {
            why = Skip::NoUpdate;
        } else if (!remoteMapPose(c, pose)) {
            why = Skip::BadPose;
        } else {
            out.push_back({id, c.colorR, c.colorG, c.colorB, pose, false});
            continue;
        }
        skipped.push_back({id, why});
    }
    for (const TestClient& t : sTestClients) {
        out.push_back({t.clientId, t.r, t.g, t.b, {t.x, t.y, t.z, t.angle, t.roomNo}, true});
    }
}

// The exact colour; the outline is dark for light colours and light for dark ones. Dimming only
// lowers alpha.
void pickColours(const Source& s, u8 alpha, bool otherFloor, Drawn& d) {
    const f32 dim = otherFloor ? kOtherFloorAlpha : 1.0f;
    const u8 a = scaled(alpha, dim);
    d.fill[0] = s.r;
    d.fill[1] = s.g;
    d.fill[2] = s.b;
    d.fill[3] = a;
    const int luma = (299 * s.r + 587 * s.g + 114 * s.b) / 1000;
    const u8 o = luma >= 128 ? 16 : 240;
    d.outline[0] = o;
    d.outline[1] = o;
    d.outline[2] = static_cast<u8>(o + 4);
    d.outline[3] = a;
}

// Tip, base-left, base-right in screen space.
void arrowVerts(Vec2 anchor, Vec2 dir, f32 height, Vec2 out[3]) {
    const Vec2 perp = {-dir.y, dir.x};
    const Vec2 base = anchor - dir * (kBase * height);
    out[0] = anchor + dir * (kTip * height);
    out[1] = base - perp * (kHalfWidth * height);
    out[2] = base + perp * (kHalfWidth * height);
}

// The triangle scaled about its incentre so every edge moves out by exactly `width`.
bool expandTri(const Vec2 v[3], f32 width, Vec2 out[3]) {
    const f32 a = length(v[1] - v[2]);
    const f32 b = length(v[2] - v[0]);
    const f32 c = length(v[0] - v[1]);
    const f32 perimeter = a + b + c;
    const f32 area2 = std::fabs(cross(v[1] - v[0], v[2] - v[0]));
    if (perimeter <= 1e-6f || area2 <= 1e-6f) {
        return false;
    }
    const Vec2 incentre = (v[0] * a + v[1] * b + v[2] * c) * (1.0f / perimeter);
    const f32 inradius = area2 / perimeter;
    const f32 k = (inradius + width) / inradius;
    for (int i = 0; i < 3; i++) {
        out[i] = incentre + (v[i] - incentre) * k;
    }
    return true;
}

// Moves an anchor outside the rect onto the rect inset by `margin`, along the ray from the rect
// centre, pointing along that ray. False when inside.
bool pinToRect(f32 x, f32 y, f32 w, f32 h, f32 margin, Vec2& anchor, Vec2& dir) {
    if (anchor.x >= x && anchor.x <= x + w && anchor.y >= y && anchor.y <= y + h) {
        return false;
    }
    const Vec2 centre = {x + w * 0.5f, y + h * 0.5f};
    const f32 halfW = std::max(w * 0.5f - margin, 0.5f);
    const f32 halfH = std::max(h * 0.5f - margin, 0.5f);
    const Vec2 v = anchor - centre;
    constexpr f32 kInf = std::numeric_limits<f32>::infinity();
    const f32 sx = std::fabs(v.x) > 1e-6f ? halfW / std::fabs(v.x) : kInf;
    const f32 sy = std::fabs(v.y) > 1e-6f ? halfH / std::fabs(v.y) : kInf;
    anchor = centre + v * std::min(sx, sy);
    dir = normalized(v);
    return true;
}

void emit(Vec2 p, const u8 c[4]) {
    GXPosition3f32(p.x, p.y, 0.0f);
    GXColor4u8(c[0], c[1], c[2], c[3]);
}

// A triangle in `fill` with a ring in `outline` around it; the fill never overlaps the ring, so
// a translucent marker still shows its own colour.
void drawOutlinedTri(const Vec2 v[3], const u8 fill[4], const u8 outline[4]) {
    Vec2 o[3];
    if (!expandTri(v, kOutline, o)) {
        return;
    }
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 21);
    for (int i = 0; i < 3; i++) {
        const int j = (i + 1) % 3;
        emit(v[i], outline);
        emit(v[j], outline);
        emit(o[j], outline);
        emit(v[i], outline);
        emit(o[j], outline);
        emit(o[i], outline);
    }
    for (int i = 0; i < 3; i++) {
        emit(v[i], fill);
    }
    GXEnd();
}

void drawPlainTri(const Vec2 v[3], const u8 c[4]) {
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) {
        emit(v[i], c);
    }
    GXEnd();
}

void drawMarker(const Drawn& d) {
    Vec2 v[3];
    arrowVerts(d.anchor, d.dir, d.height, v);
    drawOutlinedTri(v, d.fill, d.outline);
    if (d.floorDelta != 0) {
        // A small chevron beside the arrow: up for a floor above ours, down for one below.
        const f32 s = std::max(d.height * 0.5f, 4.0f);
        const Vec2 c = d.anchor + Vec2{d.height * 0.8f, -d.height * 0.55f};
        const f32 dy = d.floorDelta > 0 ? -1.0f : 1.0f;
        const Vec2 chevron[3] = {c + Vec2{0.0f, dy * s * 0.5f},
            c + Vec2{-s * 0.5f, -dy * s * 0.35f}, c + Vec2{s * 0.5f, -dy * s * 0.35f}};
        drawOutlinedTri(chevron, d.fill, d.outline);
    }
}

// Direct POS + CLR0, PASSCLR, alpha blending, as J2DGrafContext::fillBox sets up.
void beginPrims() {
    dComIfGp_getCurrentGrafPort()->setup2D();
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
}

void endPrims() {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_RGBA4, 0);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

bool circlesOverlap(Vec2 a, f32 ra, Vec2 b, f32 rb) {
    return length(a - b) < ra + rb;
}

// Keeps our own arrow on top: drawn again in its palette colour where a marker overlaps it.
void drawLocalOnTop(Frame& f) {
    if (daPy_getPlayerActorClass() == nullptr || dMap_HIO_prm_res_dst_s::m_res == nullptr) {
        return;
    }
    const f32 localRadius = kTip * f.localHeight;
    bool overlapped = false;
    for (const Drawn& d : f.cursors) {
        overlapped = overlapped ||
                     circlesOverlap(f.localAnchor, localRadius, d.anchor, kTip * d.height + kOutline);
    }
    if (!overlapped) {
        return;
    }
    GXColor c;
    dMpath_ColorCnv_n::convertRGB5A3_To_GXColor(
        c, dMap_HIO_prm_res_dst_s::m_res->palette_data[kLocalCursorGroup].field_0x0);
    const u8 col[4] = {c.r, c.g, c.b, scaled(c.a, f.mapAlpha / 255.0f)};
    Vec2 v[3];
    arrowVerts(f.localAnchor, f.localDir, f.localHeight, v);
    drawPlainTri(v, col);
    f.localRedrawn = true;
}

// Once per game tick; the painter may run several times per tick with frame interpolation.
uint32_t nowTick() {
    return g_Counter.mCounter0;
}

void beginFrame(Frame& f, Surface s) {
    f.surface = s;
    f.valid = true;
    f.tick = nowTick();
    f.gate = Gate::Drawn;
    f.localRedrawn = false;
    f.cursors.clear();
    f.skipped.clear();
}

std::vector<Source> sSources;

// What collectPauseDmap gathered this tick, drawn by drawPauseDmap.
struct PauseEntry {
    Drawn drawn;
    f32 alphaRate = 0.0f;
    Vec2 pos;  // on the map pane
};
struct PauseList {
    bool valid = false;
    uint32_t tick = 0;
    Gate gate = Gate::Drawn;
    std::vector<Source> sources;
    std::vector<Skipped> skipped;
    std::vector<PauseEntry> entries;
};
PauseList sPause;

}  // namespace

const char* gateName(Gate g) {
    switch (g) {
    case Gate::Drawn: return "drawn";
    case Gate::AlphaZero: return "alphaZero";
    case Gate::NotConnected: return "notConnected";
    case Gate::LocationsOff: return "locationsOff";
    case Gate::CutsceneHidden: return "cutscene";
    case Gate::NoStage: return "noStage";
    }
    return "?";
}

const char* skipName(Skip s) {
    switch (s) {
    case Skip::NoSave: return "noSave";
    case Skip::NoUpdate: return "noUpdate";
    case Skip::OtherStage: return "otherStage";
    case Skip::OtherLayer: return "otherLayer";
    case Skip::OtherFloor: return "otherFloor";
    case Skip::BadPose: return "badPose";
    }
    return "?";
}

// The offscreen pass: lookAt(eye=(cx, cz, -5000), at=(cx, cz, 5000), up=(0, -1, 0)) and an ortho
// of +-worldW/2 (negated when mirrored) by +-worldH/2 over the whole texture.
Vec2 projectMinimap(const MinimapXform& t, f32 x, f32 z) {
    const f32 right = (t.mirror ? -0.5f : 0.5f) * t.worldW;
    const f32 top = 0.5f * t.worldH;
    const f32 ndcX = (x - t.centerX) / right;
    const f32 ndcY = -(z - t.centerZ) / top;
    return {t.rectX + (ndcX + 1.0f) * 0.5f * t.rectW, t.rectY + (1.0f - ndcY) * 0.5f * t.rectH};
}

// The analytic derivative of projectMinimap along the facing (sin a, cos a).
Vec2 facingMinimap(const MinimapXform& t, s16 angle) {
    const f32 sx = (t.mirror ? -t.rectW : t.rectW) / t.worldW;
    const f32 sy = t.rectH / t.worldH;
    return normalized({cM_ssin(angle) * sx, cM_scos(angle) * sy});
}

void drawMinimap(dMap_c& map, f32 x, f32 y, f32 w, f32 h, u8 alpha) {
    Frame& f = sFrames[size_t(Surface::Minimap)];
    beginFrame(f, Surface::Minimap);
    f.mapAlpha = alpha;
    MinimapXform& t = f.xform;
    // What entry() stored for this frame's offscreen pass, which ran before 2DOpa.
    t.centerX = map.mPosX;
    t.centerZ = map.mPosZ;
    t.worldW = map.field_0x8;
    t.worldH = map.field_0xc;
    t.texW = map.mTexWidth;
    t.texH = map.mTexHeight;
    t.mirror = mirrorMode();
    t.rectX = x;
    t.rectY = y;
    t.rectW = w;
    t.rectH = h;
    t.cursorSize = map.getPlayerCursorSize();
    f.rectOnScreen = x + w > mDoGph_gInf_c::getMinXF() && x < mDoGph_gInf_c::getMaxXF();

    if (t.worldW <= 0.0f || t.worldH <= 0.0f || t.texW == 0 || t.texH == 0) {
        f.valid = false;
        return;
    }
    const f32 unitsPerTexel = w / t.texW;
    const Vec localPos = dMapInfo_n::getMapPlayerPos();
    f.local = {localPos.x, localPos.y, localPos.z, dMapInfo_n::getMapPlayerAngleY(),
        static_cast<int8_t>(dComIfGp_roomControl_getStayNo())};
    f.localAnchor = projectMinimap(t, localPos.x, localPos.z);
    f.localDir = facingMinimap(t, f.local.angle);
    f.localHeight = t.cursorSize * unitsPerTexel;

    if (alpha == 0) {
        f.gate = Gate::AlphaZero;
        return;
    }
    gatherSources(sSources, f.skipped, f.gate);
    if (f.gate != Gate::Drawn) {
        return;
    }

    // Floors only mean something on dungeon maps (stay type 1) with a floor height.
    stage_stag_info_class* stag = dComIfGp_getStageStagInfo();
    const bool floors =
        map.getStayType() == 1 && stag != nullptr && dStage_stagInfo_GetGapLevel(stag) > 0;
    const f32 height = std::max(t.cursorSize * unitsPerTexel * kRemoteScale, kMinArrow);
    for (const Source& s : sSources) {
        Drawn d;
        d.clientId = s.clientId;
        d.pose = s.pose;
        d.injected = s.injected;
        if (floors) {
            d.floorDelta = static_cast<int8_t>(
                dMapInfo_c::calcFloorNo(s.pose.y, true, s.pose.roomNo) - map.mRenderedFloor);
        }
        d.anchor = projectMinimap(t, s.pose.x, s.pose.z);
        d.dir = facingMinimap(t, s.pose.angle);
        d.height = height;
        // Never culled: a teammate in another room stays findable at the map's edge.
        const f32 pinnedHeight = height * kPinScale;
        if (pinToRect(x, y, w, h, kTip * pinnedHeight + kOutline, d.anchor, d.dir)) {
            d.pinned = true;
            d.height = pinnedHeight;
        }
        d.tip = d.anchor + d.dir * (kTip * d.height);
        pickColours(s, alpha, d.floorDelta != 0, d);
        f.cursors.push_back(d);
    }
    if (f.cursors.empty()) {
        return;
    }

    GXPushDebugGroup("twili map cursors");
    beginPrims();
    for (const Drawn& d : f.cursors) {
        drawMarker(d);
    }
    drawLocalOnTop(f);
    endPrims();
    GXPopDebugGroup();
}

void collectPauseDmap(s8 floorNo, f32 alphaRate, PauseCnvFn cnv, const void* ctx) {
    // Both blended floors come in the same tick, and the create-time call before the menu's
    // first _move, so a new tick starts a new list.
    const uint32_t tick = nowTick();
    if (!sPause.valid || sPause.tick != tick) {
        sPause.valid = true;
        sPause.tick = tick;
        sPause.entries.clear();
        gatherSources(sPause.sources, sPause.skipped, sPause.gate);
    }
    // The floor the blend has fully left: its players count as on another floor.
    if (alphaRate <= 0.0f) {
        return;
    }
    for (const Source& s : sPause.sources) {
        if (dMapInfo_c::calcFloorNo(s.pose.y, true, s.pose.roomNo) != floorNo) {
            continue;
        }
        // The same floor twice (no blend running) adds up to one full-alpha marker.
        auto it = std::find_if(sPause.entries.begin(), sPause.entries.end(),
            [&](const PauseEntry& e) { return e.drawn.clientId == s.clientId; });
        if (it != sPause.entries.end()) {
            it->alphaRate = std::min(it->alphaRate + alphaRate, 1.0f);
            continue;
        }
        PauseEntry e;
        e.drawn.clientId = s.clientId;
        e.drawn.pose = s.pose;
        e.drawn.injected = s.injected;
        e.drawn.fill[0] = s.r;
        e.drawn.fill[1] = s.g;
        e.drawn.fill[2] = s.b;
        e.alphaRate = alphaRate;
        cnv(ctx, s.pose.x, s.pose.z, &e.pos.x, &e.pos.y);
        // A long step: the conversion is linear and applies mirror mode itself.
        Vec2 ahead;
        cnv(ctx, s.pose.x + cM_ssin(s.pose.angle) * 1000.0f,
            s.pose.z + cM_scos(s.pose.angle) * 1000.0f, &ahead.x, &ahead.y);
        e.drawn.dir = normalized(ahead - e.pos);
        sPause.entries.push_back(e);
    }
}

void drawPauseDmap(dMenuMapCommon_c& common, f32 originX, f32 originY, f32 alpha) {
    Frame& f = sFrames[size_t(Surface::PauseDmap)];
    beginFrame(f, Surface::PauseDmap);
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    f.mapAlpha = scaled(255, alpha);
    f.rectOnScreen = true;
    // Only what this tick (or the one before, with interpolated frames between) collected.
    if (!sPause.valid || nowTick() - sPause.tick > 1) {
        f.valid = false;
        return;
    }
    f.gate = sPause.gate;
    if (f.gate == Gate::Drawn && alpha <= 0.0f) {
        f.gate = Gate::AlphaZero;
    }
    if (f.gate != Gate::Drawn) {
        return;
    }
    f.skipped = sPause.skipped;
    for (const Source& s : sPause.sources) {
        const bool shown = std::any_of(sPause.entries.begin(), sPause.entries.end(),
            [&](const PauseEntry& e) { return e.drawn.clientId == s.clientId; });
        if (!shown) {
            f.skipped.push_back({s.clientId, Skip::OtherFloor});
        }
    }

    // Sized like the Link icon, which follows the menu's zoom.
    f32 iconScale = 1.0f;
    if (J2DPicture* link = common.mPictures[ICON_LINK_e]) {
        iconScale = link->getScaleX();
    }
    const f32 height = std::max(kPauseArrow * iconScale * kRemoteScale, kMinArrow);
    const bool mirror = mirrorMode();
    for (const PauseEntry& e : sPause.entries) {
        Drawn d = e.drawn;
        d.anchor = {originX + e.pos.x, originY + e.pos.y};
        // drawIcon's own mirror step.
        if (mirror) {
            d.anchor.x = common.getMirrorCenterPosX(d.anchor.x, 0.0f);
        }
        d.height = height;
        d.tip = d.anchor + d.dir * (kTip * d.height);
        Source s;
        s.r = e.drawn.fill[0];
        s.g = e.drawn.fill[1];
        s.b = e.drawn.fill[2];
        pickColours(s, scaled(255, alpha * e.alphaRate), false, d);
        f.cursors.push_back(d);
    }
    if (f.cursors.empty()) {
        return;
    }

    GXPushDebugGroup("twili pause map cursors");
    beginPrims();
    for (const Drawn& d : f.cursors) {
        drawMarker(d);
    }
    endPrims();
    GXPopDebugGroup();
}

const Frame& lastFrame(Surface s) {
    return sFrames[size_t(s)];
}

void setTestClients(std::vector<TestClient> clients) {
    sTestClients = std::move(clients);
}

void reset() {
    sTestClients.clear();
    sPause = {};
    for (Frame& f : sFrames) {
        f = {};
    }
}

bool selfTest(std::string& failure) {
    const auto fail = [&](std::string msg) {
        failure = std::move(msg);
        return false;
    };

    // The projection against the real matrix functions, both mirror modes.
    for (int mirror = 0; mirror < 2; mirror++) {
        MinimapXform t;
        t.centerX = 1234.5f;
        t.centerZ = -8765.25f;
        t.worldW = 144.0f * 75.0f;
        t.worldH = 144.0f * 75.0f;
        t.texW = t.texH = 144;
        t.mirror = mirror != 0;
        t.rectX = 70.0f;
        t.rectY = 250.0f;
        t.rectW = t.rectH = 144.0f;

        Mtx view;
        Vec eye = {t.centerX, t.centerZ, -5000.0f};
        Vec at = {t.centerX, t.centerZ, 5000.0f};
        Vec up = {0.0f, -1.0f, 0.0f};
        mDoMtx_lookAt(view, &eye, &at, &up, 0);
        Mtx44 proj;
        const f32 right = (t.mirror ? -0.5f : 0.5f) * t.worldW;
        const f32 top = 0.5f * t.worldH;
        C_MTXOrtho(proj, top, -top, -right, right, 0.0f, 10000.0f);

        for (int i = 0; i < 64; i++) {
            const f32 x = t.centerX + (static_cast<f32>((i * 37) % 64) - 32.0f) * 211.0f;
            const f32 z = t.centerZ + (static_cast<f32>((i * 53) % 64) - 32.0f) * 197.0f;
            Vec p = {x, z, 0.0f};
            Vec v;
            MTXMultVec(view, &p, &v);
            const f32 ndcX = proj[0][0] * v.x + proj[0][1] * v.y + proj[0][3];
            const f32 ndcY = proj[1][0] * v.x + proj[1][1] * v.y + proj[1][3];
            const Vec2 want = {
                t.rectX + (ndcX + 1.0f) * 0.5f * t.rectW, t.rectY + (1.0f - ndcY) * 0.5f * t.rectH};
            const Vec2 got = projectMinimap(t, x, z);
            if (length(got - want) > 1e-2f) {
                return fail(fmt::format("projection mirror={} ({:.1f}, {:.1f}): {:.3f},{:.3f} "
                                        "want {:.3f},{:.3f}",
                    mirror, x, z, got.x, got.y, want.x, want.y));
            }
            // Facing: the analytic derivative against a long finite step.
            const s16 angle = static_cast<s16>(i * 1031);
            const Vec2 ahead =
                projectMinimap(t, x + cM_ssin(angle) * 5000.0f, z + cM_scos(angle) * 5000.0f);
            const Vec2 numeric = normalized(ahead - got);
            const Vec2 analytic = facingMinimap(t, angle);
            if (length(numeric - analytic) > 1e-3f) {
                return fail(fmt::format(
                    "facing mirror={} angle={:#06x}: {:.4f},{:.4f} want {:.4f},{:.4f}", mirror,
                    static_cast<u16>(angle), analytic.x, analytic.y, numeric.x, numeric.y));
            }
        }
        // Angle 0x4000 faces +X: right on screen, left when mirrored; angle 0 faces +Z: down.
        const Vec2 east = facingMinimap(t, 0x4000);
        const Vec2 south = facingMinimap(t, 0);
        if ((t.mirror ? east.x > -0.99f : east.x < 0.99f) || south.y < 0.99f) {
            return fail(fmt::format("facing axes mirror={}: east {:.3f},{:.3f} south {:.3f},{:.3f}",
                mirror, east.x, east.y, south.x, south.y));
        }
    }

    // The outline ring is the same width along every edge.
    {
        Vec2 v[3];
        arrowVerts({100.0f, 100.0f}, normalized({0.3f, -0.8f}), 12.0f, v);
        Vec2 o[3];
        if (!expandTri(v, kOutline, o)) {
            return fail("outline: degenerate arrow");
        }
        for (int i = 0; i < 3; i++) {
            const Vec2 edge = normalized(v[(i + 1) % 3] - v[i]);
            const f32 dist = std::fabs(cross(edge, o[i] - v[i]));
            if (std::fabs(dist - kOutline) > 1e-3f) {
                return fail(
                    fmt::format("outline edge {}: {:.4f} wide, want {:.4f}", i, dist, kOutline));
            }
        }
        if (std::fabs(length(v[0] - Vec2{100.0f, 100.0f}) - kTip * 12.0f) > 1e-3f) {
            return fail("arrow: tip distance");
        }
    }

    // Pins: inside stays put, outside lands on the inset border pointing outwards.
    {
        const f32 x = 50.0f, y = 300.0f, w = 144.0f, h = 144.0f, margin = 6.0f;
        Vec2 a = {100.0f, 350.0f};
        Vec2 d = {0.0f, 1.0f};
        if (pinToRect(x, y, w, h, margin, a, d) || a.x != 100.0f || d.y != 1.0f) {
            return fail("pin: an anchor inside the rect moved");
        }
        const Vec2 outside[] = {
            {-400.0f, 372.0f}, {900.0f, 100.0f}, {122.0f, 2000.0f}, {-50.0f, -50.0f}};
        for (Vec2 p : outside) {
            a = p;
            if (!pinToRect(x, y, w, h, margin, a, d)) {
                return fail("pin: an anchor outside the rect was not pinned");
            }
            const bool inside = a.x >= x + margin - 1e-3f && a.x <= x + w - margin + 1e-3f &&
                                a.y >= y + margin - 1e-3f && a.y <= y + h - margin + 1e-3f;
            const Vec2 out = normalized(p - Vec2{x + w * 0.5f, y + h * 0.5f});
            if (!inside || length(d - out) > 1e-4f) {
                return fail(fmt::format("pin ({:.0f}, {:.0f}): at {:.2f},{:.2f}", p.x, p.y, a.x, a.y));
            }
        }
    }

    // Colours: the fill is the exact colour at the map's alpha; outlines contrast.
    {
        Source s;
        s.r = 0x13;
        s.g = 0xC7;
        s.b = 0x6D;
        Drawn d;
        pickColours(s, 200, false, d);
        if (d.fill[0] != 0x13 || d.fill[1] != 0xC7 || d.fill[2] != 0x6D || d.fill[3] != 200) {
            return fail("colours: fill is not the exact colour");
        }
        pickColours(s, 200, true, d);
        if (d.fill[3] != scaled(200, kOtherFloorAlpha) || d.fill[0] != 0x13) {
            return fail("colours: other-floor dim");
        }
        s.r = s.g = s.b = 255;
        pickColours(s, 255, false, d);
        const u8 dark = d.outline[0];
        s.r = s.g = s.b = 0;
        pickColours(s, 255, false, d);
        if (dark >= 64 || d.outline[0] <= 192) {
            return fail("colours: outline does not contrast with white/black");
        }
    }
    return true;
}

}  // namespace twili::ui::map_cursor
